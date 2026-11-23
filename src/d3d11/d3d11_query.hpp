#pragma once
#include "com/com_guid.hpp"
#include "d3d11_device_child.hpp"
#include "dxmt_occlusion_query.hpp"
#include "log/log.hpp"
#include "../d3d10/d3d10_query.hpp"
#include <mutex>

namespace dxmt {

template <typename Query>
class MTLD3DQueryBase : public MTLD3D11DeviceChild<Query> {
public:
  MTLD3DQueryBase(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *desc)
      : MTLD3D11DeviceChild<Query>(pDevice), desc_(*desc),
        d3d10_(this, pDevice->GetImmediateContextPrivate()) {}
  ~MTLD3DQueryBase(){};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11Query)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10DeviceChild) || riid == __uuidof(ID3D10Query)) {
      *ppvObject = ref(&d3d10_);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11Query), riid)) {
      Logger::warn("D3D11Query::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  };

  void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC *pDesc) final {
    if (pDesc) {
      pDesc->Query = desc_.Query;
      pDesc->MiscFlags = desc_.MiscFlags;
    }
  };

  void STDMETHODCALLTYPE GetDesc1(D3D11_QUERY_DESC1 *pDesc) final {
    if (pDesc) {
      *pDesc = desc_;
    }
  };

protected:
  D3D11_QUERY_DESC1 desc_;
  MTLD3D10Query d3d10_;
};

template <typename DataType>
class MTLD3D11DummyQuery : public MTLD3DQueryBase<ID3D11Query1> {
public:
  MTLD3D11DummyQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *desc)
      : MTLD3DQueryBase<ID3D11Query1>(pDevice, desc) {}

  virtual UINT STDMETHODCALLTYPE GetDataSize() override {
    return sizeof(DataType);
  };
};

/**

From D3D11.3 Functional Spec:

- Passing IssueFlags with only the D3DISSUE_BEGIN bit set causes the Query to
enter the "building" state (regardless of whatever state it was in before)

- A second D3DISSUE_BEGIN will result in the range being reset (the first
D3DISSUE_BEGIN is effectively discarded/ ignored).

- Some Query Types only support D3DISSUE_END

- When the Query is in the "signaled" state, the Query Type supports
D3DISSUE_BEGIN, and Issue is invoked with just the D3DISSUE_END flag: it is
equivalent to invoking Issue with both D3DISSUE_BEGIN and D3DISSUE_END bits set,
as well as being equivalent to an invocation of Issue with D3DISSUE_BEGIN
followed immediately by another invocation of Issue with D3DISSUE_END

- (BEGIN and END) define a bracketing of graphics commands

- Bracketings of Queries are allowed to overlap and nest.
 */
enum class QueryState {
  /**
    After DeviceContext::Begin
   */
  Building,
  /**
    After DeviceContext::End
   */
  Issued,
  /**
    Default state, or when data is ready
   */
  Signaled,
  Undefined,
};

enum class EventState {
  Pending,
  Signaled,
  Stall,
  Invalid,
};

constexpr size_t kEventStallThreshold = 64;

/**
OBELISK_DXMT_QUERY_RESCUE=once in the environment, read once per process; anything else, or nothing, is off.

Off: an event or disjoint query that was not there kEventStallThreshold looks in a row flushes at each look from then on,
whatever its caller asked for with D3D11_ASYNC_GETDATA_DONOTFLUSH, until the GPU has signalled it.

Once: such a look flushes only while the query's marker is in a chunk that has not been handed to the encoder (a caller that does
nothing but look would wait forever); from there on the marker reaches the GPU by itself, and the look says S_FALSE.
*/
bool QueryRescueOnce();

/* The device's lock, taken with the switch above and not without it. */
template <typename Mutex>
std::unique_lock<Mutex>
LockForRescue(Mutex &mutex) {
  std::unique_lock<Mutex> lock(mutex, std::defer_lock);
  if (QueryRescueOnce())
    lock.lock();
  return lock;
}

struct MTLD3D11EventQuery : public ID3D11Query1 {
  virtual void Issue(uint64_t current_seq_id) = 0;
  virtual EventState CheckEventState(uint64_t coherent_seq_id) = 0;
  /* The event the query was last issued with, while looks at it stall; 0 otherwise. */
  virtual uint64_t StalledEventSeqId() = 0;
};

template <typename DataType>
class MTLD3D11EventQueryImpl : public MTLD3DQueryBase<MTLD3D11EventQuery> {
public:
  MTLD3D11EventQueryImpl(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *desc)
      : MTLD3DQueryBase<MTLD3D11EventQuery>(pDevice, desc) {}

  UINT STDMETHODCALLTYPE GetDataSize() override { return sizeof(DataType); };

  void Issue(uint64_t current_seq_id) override {
    state = QueryState::Issued;
    should_be_signaled_at = current_seq_id;
    stall_counter = 0;
  }

  EventState CheckEventState(uint64_t coherent_seq_id) override {
    if (state == QueryState::Undefined)
      return EventState::Invalid;
    if (should_be_signaled_at <= coherent_seq_id) {
      state = QueryState::Signaled;
      stall_counter = 0;
      return EventState::Signaled;
    }
    if (stall_counter != ~0u) {
      if (stall_counter < kEventStallThreshold) {
        stall_counter++;
        return EventState::Pending;
      }
      stall_counter = ~0u;
    }
    return EventState::Stall;
  };

  uint64_t StalledEventSeqId() override {
    return state == QueryState::Issued && stall_counter == ~0u ? should_be_signaled_at : 0;
  }

private:
  QueryState state = QueryState::Undefined;
  uint32_t stall_counter = 0;
  uint64_t should_be_signaled_at = 0;
};

class MTLD3D11OcclusionQuery : public ID3D11Query1 {
public:
  virtual HRESULT GetData(void *data) = 0;
  virtual VisibilityResultQuery *Begin() = 0;
  virtual VisibilityResultQuery *End() = 0;
  virtual void DoDeferredQuery(VisibilityResultQuery *deferred_query) = 0;
};

HRESULT CreateOcclusionQuery(MTLD3D11Device *pDevice,
                              const D3D11_QUERY_DESC1 *pDesc,
                              ID3D11Query1 **ppQuery);

class MTLD3D11TimestampQuery : public ID3D11Query1 {
public:
  virtual HRESULT GetData(void *data) = 0;
  virtual TimestampQuery *End() = 0;
};

HRESULT CreateTimestampQuery(MTLD3D11Device *pDevice,
                              const D3D11_QUERY_DESC1 *pDesc,
                              ID3D11Query1 **ppQuery);

} // namespace dxmt