#include "d3d11_query_rescue.hpp"
#include "dxmt_command_queue.hpp"
#include "util_env.hpp"
#include <atomic>

namespace dxmt {

QueryArm
ChooseQueryArm() {
  static const bool once = env::getEnvVar("OBELISK_DXMT_QUERY_RESCUE") == "once";
  return {once, otl_active() != 0};
}

template <typename DataType>
static ID3D11Query1 *
NewEventQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, QueryArm arm) {
  if (arm.once)
    return arm.timed ? ref<ID3D11Query1>(new EventQueryOnceTimed<DataType>(pDevice, pDesc))
                     : ref<ID3D11Query1>(new MTLD3D11EventQueryOnce<DataType>(pDevice, pDesc));
  return arm.timed ? ref<ID3D11Query1>(new EventQueryTimed<DataType>(pDevice, pDesc))
                   : ref<ID3D11Query1>(new MTLD3D11EventQueryImpl<DataType>(pDevice, pDesc));
}

HRESULT
CreateEventQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, QueryArm arm, ID3D11Query1 **ppQuery) {
  *ppQuery = pDesc->Query == D3D11_QUERY_EVENT ? NewEventQuery<BOOL>(pDevice, pDesc, arm)
                                               : NewEventQuery<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(pDevice, pDesc, arm);
  return S_OK;
}

bool
QuerySampled(uint64_t now) {
  static std::atomic<uint64_t> taken = 0; // the 100 ms << 8 | how many of them were
  const uint64_t of = now / 100000000 << 8;
  uint64_t was = taken.load(std::memory_order_relaxed);
  while (was < of || (was & 0xff) < kQuerySamples)
    if (taken.compare_exchange_weak(was, was < of ? of + 1 : was + 1, std::memory_order_relaxed))
      return true;
  return false;
}

static void
TraceSeen(QueryTrace &trace, CommandQueue &queue, uint64_t now, uint32_t how) {
  if (trace.sampled)
    otl_put(now, OTL_D_QUERY_SEEN, how, queue.otl_id_, trace.id << 32 | trace.generation, trace.first, trace.last, trace.looks,
            trace.event);
  trace.sampled = false;
  trace.seen = true;
}

void
TraceIssue(QueryTrace &trace, CommandQueue &queue, uint32_t kind, uint64_t event_id) {
  const uint64_t now = otl_now();
  std::lock_guard<dxmt::mutex> lock(trace.lock);
  if (trace.ended && !trace.seen)
    TraceSeen(trace, queue, now, 1);
  trace.generation++;
  trace.ended = true;
  trace.seen = false;
  trace.event = event_id;
  trace.first = trace.last = 0;
  trace.looks = 0;
  trace.sampled = otl_active() && QuerySampled(now);
  if (trace.sampled)
    otl_put(now, OTL_D_QUERY, kind | otl_query_end, queue.otl_id_, trace.id << 32 | trace.generation, event_id, queue.CurrentSeqId(),
            queue.otl_group_, 0);
}

void
TraceLook(QueryTrace &trace, CommandQueue &queue, uint32_t generation, uint64_t began, uint64_t ended, HRESULT hr) {
  std::lock_guard<dxmt::mutex> lock(trace.lock);
  if (trace.generation != generation || trace.seen || !trace.ended)
    return; // (ended again meanwhile, or found there by another look)
  trace.looks++;
  if (!trace.first)
    trace.first = began;
  if (hr == S_OK) {
    TraceSeen(trace, queue, ended, 0);
    return;
  }
  trace.last = ended;
  if (trace.sampled && trace.looks == kEventStallThreshold + 1)
    otl_put(ended, OTL_D_QUERY_SEEN, 2, queue.otl_id_, trace.id << 32 | trace.generation, trace.first, trace.last, trace.looks, trace.event);
}

} // namespace dxmt
