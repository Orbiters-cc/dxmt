/*
 * SPDX-License-Identifier: MIT
 *
 * DXMT's part of the host timeline (otl.h), in the native build only: elsewhere everything here is nothing.
 * Off unless OBELISK_TIMELINE is set for the process; on, it changes nothing of what is encoded, but that with
 * OBELISK_TIMELINE_PASSES=N the encoders of every Nth frame are listed and take GPU timestamps at their stage boundaries.
 *
 * What owns a command buffer's samples (src/winemetal/unix/otl_samples.h): the queue has one pool of sample buffers. The
 * thread that encodes takes a free one for the command buffer and gives it to the chunk (dxmt_otl_encoded()); the chunk
 * goes to the finish thread as it always did, which gives it, done, to the pool's own thread; that one resolves it, says
 * what it held and only then makes it free again. One holder at a time, handed over through what already orders the
 * threads; nobody looks for a sample buffer by what it is for.
 */
#pragma once

#include <stdint.h>

#ifdef DXMT_NATIVE

#include "otl.h"

#ifdef __cplusplus
extern "C" {
#endif
struct otl_pool;
struct otl_samples;
/* What a queue of `device` (an id<MTLDevice>) is in the timeline from here on: its id, and its pool when there is one. */
uint64_t dxmt_otl_queue(uint64_t device, struct otl_pool **pool);
/* The queue ends: its pool goes once what is still out of it came back. */
void dxmt_otl_queue_ended(uint64_t queue, struct otl_pool *pool);
/* This thread encodes the command buffer of `chunk` of `queue` from now on; its encoders are recorded with `recorded`. */
void dxmt_otl_encoding(uint64_t queue, uint64_t chunk, int recorded, struct otl_pool *pool);
/* It has: its samples, NULL when it took none. */
struct otl_samples *dxmt_otl_encoded(void);
/* The command buffer `cmdbuf` (an id<MTLCommandBuffer>) of `samples` was seen done, or (`done` 0) never will be by the caller.
 * Returns at once. */
void dxmt_otl_samples_done(struct otl_samples *samples, uint64_t cmdbuf, int done);
/* This thread's next commit is the initializer queue's, with this event value (0: it is not). */
void dxmt_otl_initializer(uint64_t value);
/* The time this thread has waited for pipelines since the last call. */
uint64_t dxmt_otl_take_pipeline_wait(void);
/* This thread waited for a pipeline of `kind` from `start` until now. */
void dxmt_otl_pipeline_wait(uint32_t kind, uint64_t start);
/* The render pass this thread begins next was kept from joining a later one by this (enum otl_stop). */
void dxmt_otl_stop(uint32_t stop);
#ifdef __cplusplus
}
#endif

#else

struct otl_pool;
struct otl_samples;
static const int otl_passes = 0;
static inline int otl_active(void) { return 0; }
static inline uint64_t otl_now(void) { return 0; }
static inline uint64_t otl_id(void) { return 0; }
static inline void otl_init(const char *) {}
static inline void otl_put(uint64_t, uint16_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
static inline void otl_hist(int, uint64_t) {}
static inline uint64_t dxmt_otl_queue(uint64_t, struct otl_pool **) { return 0; }
static inline void dxmt_otl_queue_ended(uint64_t, struct otl_pool *) {}
static inline void dxmt_otl_encoding(uint64_t, uint64_t, int, struct otl_pool *) {}
static inline struct otl_samples *dxmt_otl_encoded(void) { return 0; }
static inline void dxmt_otl_samples_done(struct otl_samples *, uint64_t, int) {}
static inline void dxmt_otl_initializer(uint64_t) {}
static inline uint64_t dxmt_otl_take_pipeline_wait(void) { return 0; }
static inline void dxmt_otl_pipeline_wait(uint32_t, uint64_t) {}
static inline void dxmt_otl_stop(uint32_t) {}
static inline uint64_t otl_attachment(uint32_t, uint32_t, uint32_t, uint32_t, int, int, uint32_t, uint32_t) { return 0; }
enum {
  OTL_D_READY, OTL_D_ENCODE, OTL_D_DONE, OTL_D_SIGNAL, OTL_D_SEOC, OTL_D_INIT, OTL_D_COMMIT, OTL_D_PASS_ENDED,
  OTL_D_STALL, OTL_D_QUERIES, OTL_D_QUERY, OTL_D_QUERY_SEEN, OTL_D_LOOKS,
  OTL_H_A, OTL_H_B, OTL_H_C, OTL_H_D,
};
enum otl_why {
  OTL_WHY_OTHER, OTL_WHY_TARGETS, OTL_WHY_HAZARD, OTL_WHY_CLEAR_COLOUR, OTL_WHY_CLEAR_DEPTH, OTL_WHY_CLEAR_VIEW, OTL_WHY_CLEAR_UAV,
  OTL_WHY_DRAW, OTL_WHY_DISPATCH, OTL_WHY_COPY, OTL_WHY_UPLOAD, OTL_WHY_READBACK, OTL_WHY_RESOLVE, OTL_WHY_END_EVENT,
  OTL_WHY_END_TIMESTAMP, OTL_WHY_END_OCCLUSION, OTL_WHY_GETDATA, OTL_WHY_GETDATA_STALL, OTL_WHY_FLUSH, OTL_WHY_SIGNAL, OTL_WHY_WAIT,
  OTL_WHY_MAP_STAGING, OTL_WHY_KEYED_MUTEX, OTL_WHY_COMMAND_LIST, OTL_WHY_IDLE, OTL_WHY_PRESENT, OTL_WHY_STATE,
  OTL_WHY_STAGING_WRITE, OTL_WHY_UPSCALE,
};
enum otl_stop {
  OTL_STOP_END = 0, OTL_STOP_RENDER, OTL_STOP_COMPUTE, OTL_STOP_BLIT, OTL_STOP_CLEAR, OTL_STOP_RESOLVE, OTL_STOP_EVENT,
  OTL_STOP_TIMESTAMP, OTL_STOP_OTHER, OTL_STOP_SAME = 16,
};

#endif

#ifdef __cplusplus
/* What ends a pass or commits a chunk on this thread where nothing nearer says it (enum otl_why): the call it is in, set by
 * TimedImmediateContext (d3d11_context_imm.cpp) alone and read while the timeline is on. */
inline thread_local uint32_t otl_call = 0;
struct OtlCall {
  const uint32_t former = otl_call;
  explicit OtlCall(uint32_t why) { otl_call = why; }
  ~OtlCall() { otl_call = former; }
};

/* Waits for a pipeline as `ready.wait(false)` does; the timeline says how long when it had to. */
template <typename Ready>
inline void
dxmt_otl_wait_pipeline(uint32_t kind, Ready &ready) {
  if (otl_active() && !ready.load(std::memory_order_acquire)) {
    uint64_t start = otl_now();
    ready.wait(false, std::memory_order_acquire);
    dxmt_otl_pipeline_wait(kind, start);
    return;
  }
  ready.wait(false, std::memory_order_acquire);
}
#endif
