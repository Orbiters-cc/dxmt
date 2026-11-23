/*
 * SPDX-License-Identifier: MIT
 *
 * DXMT's part of the host timeline (otl.h), in the native build only: elsewhere everything here is nothing.
 * Off unless OBELISK_TIMELINE is set for the process; it changes nothing of what is encoded, on or off.
 */
#pragma once

#include <stdint.h>

#ifdef DXMT_NATIVE

#include "otl.h"

#ifdef __cplusplus
extern "C" {
#endif
/* This thread encodes the command buffer of `chunk` of `queue` from now on (chunk 0: of none). */
void dxmt_otl_encoding(uint64_t queue, uint64_t chunk);
/* The command buffer of `chunk` is done: its encoders' GPU times, when they were asked for. */
void dxmt_otl_chunk_done(uint64_t queue, uint64_t chunk);
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

static inline int otl_active(void) { return 0; }
static inline uint64_t otl_now(void) { return 0; }
static inline void otl_init(const char *) {}
static inline void otl_put(uint64_t, uint16_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
static inline void otl_hist(int, uint64_t) {}
static inline void dxmt_otl_encoding(uint64_t, uint64_t) {}
static inline void dxmt_otl_chunk_done(uint64_t, uint64_t) {}
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
