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
#ifdef __cplusplus
}
#endif

#else

struct otl_pool;
struct otl_samples;
static const int otl_passes = 0;
static inline int otl_active(void) { return 0; }
static inline uint64_t otl_now(void) { return 0; }
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
enum {
  OTL_D_READY, OTL_D_ENCODE, OTL_D_DONE, OTL_D_SIGNAL, OTL_D_SEOC, OTL_D_INIT,
  OTL_H_A, OTL_H_B, OTL_H_C, OTL_H_D,
};

#endif

#ifdef __cplusplus
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
