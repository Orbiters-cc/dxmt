#pragma once
#include "d3d11_query.hpp"
#include "dxmt_otl.h"
#include "thread.hpp"
#include <mutex>

namespace dxmt {

/**
What a device does with the event and disjoint queries of its contexts, chosen once, as the device is made
(MTLD3D11DeviceImpl), by what classes its contexts and queries are. Neither: the classes of d3d11_query.hpp, d3d11_context_imm.cpp
(MTLD3D11ImmediateContext) and d3d11_context_def.cpp (MTLD3D11DeferredContext) alone, with nothing of this file in them.

Without the switch, a query that was not there kEventStallThreshold looks in a row flushes at each look from then on, whatever its
caller asked for with D3D11_ASYNC_GETDATA_DONOTFLUSH, until the GPU has signalled it.

With OBELISK_DXMT_QUERY_RESCUE=once (anything else is off), such a look flushes only while the query's marker is in a chunk that was
not handed to the encoder (a caller that does nothing but look would wait forever); from there on the marker reaches the GPU by
itself, and the look says S_FALSE. It waits for nothing meanwhile (MTLD3D11ImmediateContextOnce).
*/
struct QueryArm {
  bool once = false;
  bool timed = false; // the timeline (dxmt_otl.h) was on as the device was made
};
QueryArm ChooseQueryArm();

HRESULT CreateEventQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, QueryArm arm, ID3D11Query1 **ppQuery);

/* A query with the switch, to a look. */
class EventQueryLook {
public:
  /* CheckEventState; `stalled`: of which event this very look found it stalled, or 0. */
  virtual EventState Look(uint64_t coherent_seq_id, uint64_t &stalled) = 0;
  /* The event the query was last issued with, when looks at it stall; or 0. */
  virtual uint64_t StalledAt() = 0;
};

/* The same query, whose End and looks (from any thread, under its caller's own locks) see it whole. Nothing is waited for under it. */
template <typename DataType>
class MTLD3D11EventQueryOnce : public MTLD3D11EventQueryImpl<DataType>, public EventQueryLook {
  using Base = MTLD3D11EventQueryImpl<DataType>;

public:
  using Base::Base;

  void
  Issue(uint64_t current_seq_id) override {
    std::lock_guard<dxmt::mutex> lock(look_);
    Base::Issue(current_seq_id);
    issued_at_ = current_seq_id;
    stalled_ = false;
  }

  EventState
  CheckEventState(uint64_t coherent_seq_id) override {
    uint64_t stalled;
    return Look(coherent_seq_id, stalled);
  }

  EventState
  Look(uint64_t coherent_seq_id, uint64_t &stalled) override {
    std::lock_guard<dxmt::mutex> lock(look_);
    const auto state = Base::CheckEventState(coherent_seq_id);
    stalled_ = state == EventState::Stall;
    stalled = stalled_ ? issued_at_ : 0;
    return state;
  }

  uint64_t
  StalledAt() override {
    std::lock_guard<dxmt::mutex> lock(look_);
    return stalled_ ? issued_at_ : 0;
  }

private:
  dxmt::mutex look_;
  uint64_t issued_at_ = 0;
  bool stalled_ = false;
};

/* ---- the timeline's, of a device made while it is on ---- */

/* What is known of a query's last End, for OTL_D_QUERY and OTL_D_QUERY_SEEN: a leaf lock of its own, and nothing of it decides anything. */
struct QueryTrace {
  dxmt::mutex lock;
  const uint64_t id = otl_id();
  uint32_t generation = 0, looks = 0;
  uint64_t event = 0, first = 0, last = 0;
  bool sampled = false, ended = false, seen = false;
};

/* At most this many Ends are recorded one by one in any 100 ms of a process. */
constexpr uint32_t kQuerySamples = 128;
bool QuerySampled(uint64_t now);

/* What the End in course on this thread is: the bits of OTL_D_QUERY's a. */
inline thread_local uint32_t otl_query_end = 0;

void TraceIssue(QueryTrace &trace, CommandQueue &queue, uint32_t kind, uint64_t event_id);
void TraceLook(QueryTrace &trace, CommandQueue &queue, uint32_t generation, uint64_t began, uint64_t ended, HRESULT hr);

class EventQueryTraced {
public:
  QueryTrace trace;
};

template <typename Base>
class MTLD3D11EventQueryTimed : public Base, public EventQueryTraced {
public:
  using Base::Base;

  void
  Issue(uint64_t current_seq_id) override {
    Base::Issue(current_seq_id);
    TraceIssue(trace, this->m_parent->GetDXMTDevice().queue(), this->desc_.Query == D3D11_QUERY_TIMESTAMP_DISJOINT, current_seq_id);
  }
};

/* The one class an arm's queries of a kind are of, seen as Interface (one of its bases). */
template <template <typename> typename Query, typename Interface>
Interface *
EventQueryAs(ID3D11Asynchronous *pAsync, D3D11_QUERY kind) {
  if (kind == D3D11_QUERY_EVENT)
    return static_cast<Query<BOOL> *>(pAsync);
  return static_cast<Query<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT> *>(pAsync);
}
template <typename DataType> using EventQueryTimed = MTLD3D11EventQueryTimed<MTLD3D11EventQueryImpl<DataType>>;
template <typename DataType> using EventQueryOnceTimed = MTLD3D11EventQueryTimed<MTLD3D11EventQueryOnce<DataType>>;

} // namespace dxmt
