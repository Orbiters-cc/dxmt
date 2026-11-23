/*
 * SPDX-License-Identifier: MIT
 *
 * GPU times of a chunk's encoders (OBELISK_TIMELINE_PASSES): who holds a sample buffer is in dxmt_otl.h. Objective-C without
 * ARC, part of winemetal_unix.c; tests/timeline compiles it by itself.
 *
 * A pool makes its sample buffers with the queue, of the queue's device, and frees them once the queue ended and the last
 * one out came back, on its own thread: nobody waits for that. An encoder that gets no times is counted (OTL_D_PASS_SKIP).
 */
#pragma once

#include <pthread.h>
#include "../../util/dxmt_otl.h"

#define OTL_SLOTS 16 /* (a device makes only some dozens of sample buffers) */
#define OTL_ENCODERS 1000 /* of one command buffer: 4 timestamps of 8 bytes each, a sample buffer holds 32 KiB */
#define OTL_CLOCK_NS 5000000000ull

struct otl_samples {
  struct otl_samples *next;
  struct otl_pool *pool;
  id<MTLCounterSampleBuffer> buffer;
  /* the holder's alone */
  uint64_t key;
  uint32_t encoders, skipped;
  uint32_t index[OTL_ENCODERS]; /* the encoder's index | type << 16 */
  id<MTLCommandBuffer> cmdbuf;
  int done;
  /* the pool's thread's alone: what each sample held the last time it was looked at. A sample the GPU does not write (an
   * encoder without work, or without that stage) still holds what its last holder's encoder left. */
  uint64_t was[OTL_ENCODERS * 4];
};

struct otl_pool {
  pthread_mutex_t mutex;
  pthread_cond_t turn;
  struct otl_samples *free, *done;
  struct otl_samples **done_end;
  uint32_t out; /* given out and not back */
  int ended;
  uint64_t queue;
  id<MTLDevice> device;
  uint32_t slots;
  struct otl_samples slot[OTL_SLOTS];
};

/* The timeline's: which chunk this thread encodes, how far it is, and what its open encoder holds. */
static _Thread_local struct {
  uint64_t key, initializer, pipeline_wait;
  uint32_t index, type;
  uint64_t ops, pixels, commands;
  struct otl_pool *pool;
  struct otl_samples *samples;
  uint32_t no_slot;
} otl_thread;

static void otl_clock(struct otl_pool *p) {
  MTLTimestamp cpu = 0, gpu = 0;
  uint64_t before = otl_now();
  [p->device sampleTimestamps:&cpu gpuTimestamp:&gpu];
  otl_put(before, OTL_D_CLOCK, 0, cpu, gpu, before, otl_now(), p->queue, 0);
}

static void otl_resolve(struct otl_samples *s) {
  @autoreleasepool {
    if (!s->done) [s->cmdbuf waitUntilCompleted]; /* the queue ended before it was seen done */
    const uint32_t n = s->encoders;
    if (s->skipped) otl_put(otl_now(), OTL_D_PASS_SKIP, OTL_SKIP_MANY, s->key, s->skipped, 0, 0, 0, 0);
    const int failed = [s->cmdbuf status] != MTLCommandBufferStatusCompleted;
    NSData *data = n ? [s->buffer resolveCounterRange:NSMakeRange(0, n * 4)] : nil;
    const MTLCounterResultTimestamp *t = data.bytes;
    if (n && (failed || data.length < n * 4 * sizeof(*t)))
      otl_put(otl_now(), OTL_D_PASS_SKIP, failed ? OTL_SKIP_FAILED : OTL_SKIP_RESOLVE, s->key, n, 0, 0, 0, 0);
    if (n && data.length >= n * 4 * sizeof(*t)) {
      for (uint32_t e = 0; e < n; e++) {
        const int render = s->index[e] >> 16 == 0;
        uint64_t v[4];
        for (uint32_t i = 0; i < 4; i++) {
          uint64_t now = t[e * 4 + i].timestamp, *was = &s->was[e * 4 + i];
          v[i] = now == *was || now == MTLCounterErrorValue || (i > 1 && !render) ? 0 : now;
          *was = now;
        }
        if (!failed) otl_put(otl_now(), OTL_D_PASS_TIME, s->index[e], s->key, v[0], v[1], v[2], v[3], 0);
      }
    }
    [s->cmdbuf release];
    s->cmdbuf = nil;
  }
}

static void *otl_pool_thread(void *arg) {
  struct otl_pool *p = arg;
  pthread_setname_np("otl-samples");
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); /* nobody waits for it */
  uint64_t clock = 0;
  pthread_mutex_lock(&p->mutex);
  while (!p->ended || p->out) {
    struct otl_samples *s = p->done;
    if (!s) {
      pthread_cond_wait(&p->turn, &p->mutex);
      continue;
    }
    p->done = s->next;
    if (!p->done) p->done_end = &p->done;
    pthread_mutex_unlock(&p->mutex);
    if (otl_now() - clock > OTL_CLOCK_NS) {
      otl_clock(p);
      clock = otl_now();
    }
    otl_resolve(s);
    pthread_mutex_lock(&p->mutex);
    s->next = p->free;
    p->free = s;
    p->out--;
  }
  pthread_mutex_unlock(&p->mutex);
  if (clock) otl_clock(p);
  for (uint32_t i = 0; i < p->slots; i++) [p->slot[i].buffer release];
  [p->device release];
  pthread_cond_destroy(&p->turn);
  pthread_mutex_destroy(&p->mutex);
  free(p);
  return NULL;
}

uint64_t dxmt_otl_queue(uint64_t handle, struct otl_pool **out) {
  *out = NULL;
  if (!otl_active()) return 0;
  const uint64_t queue = otl_id();
  otl_put(otl_now(), OTL_D_QUEUE, 1, queue, 0, 0, 0, 0, 0);
  if (!otl_passes) return queue;
  @autoreleasepool {
    id<MTLDevice> device = (id<MTLDevice>)handle;
    id<MTLCounterSet> timestamps = nil;
    for (id<MTLCounterSet> set in device.counterSets)
      if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) timestamps = set;
    struct otl_pool *p = timestamps && [device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]
                             ? calloc(1, sizeof(*p)) : NULL;
    if (!p) return queue;
    MTLCounterSampleBufferDescriptor *d = [[MTLCounterSampleBufferDescriptor alloc] init];
    d.counterSet = timestamps;
    d.storageMode = MTLStorageModeShared;
    d.sampleCount = OTL_ENCODERS * 4;
    p->queue = queue;
    p->device = [device retain];
    p->done_end = &p->done;
    pthread_mutex_init(&p->mutex, NULL);
    pthread_cond_init(&p->turn, NULL);
    for (; p->slots < OTL_SLOTS; p->slots++) {
      struct otl_samples *s = &p->slot[p->slots];
      s->buffer = [device newCounterSampleBufferWithDescriptor:d error:NULL];
      if (!s->buffer) break;
      s->pool = p;
      s->next = p->free;
      p->free = s;
    }
    [d release];
    pthread_t t;
    if (!p->slots || pthread_create(&t, NULL, otl_pool_thread, p)) {
      for (uint32_t i = 0; i < p->slots; i++) [p->slot[i].buffer release];
      [p->device release];
      free(p);
      return queue;
    }
    pthread_detach(t);
    otl_clock(p);
    *out = p;
  }
  return queue;
}

void dxmt_otl_queue_ended(uint64_t queue, struct otl_pool *p) {
  if (queue) otl_put(otl_now(), OTL_D_QUEUE, 0, queue, 0, 0, 0, 0, 0);
  if (!p) return;
  pthread_mutex_lock(&p->mutex);
  p->ended = 1;
  pthread_cond_signal(&p->turn);
  pthread_mutex_unlock(&p->mutex);
}

void dxmt_otl_encoding(uint64_t queue, uint64_t chunk, int recorded, struct otl_pool *pool) {
  otl_thread.key = recorded ? queue << 48 | (chunk & 0xffffffffffffull) : 0;
  otl_thread.index = 0;
  otl_thread.pool = recorded ? pool : NULL;
  otl_thread.samples = NULL;
  otl_thread.no_slot = 0;
}

struct otl_samples *dxmt_otl_encoded(void) {
  struct otl_samples *s = otl_thread.samples;
  if (otl_thread.no_slot)
    otl_put(otl_now(), OTL_D_PASS_SKIP, otl_thread.pool ? OTL_SKIP_SLOT : OTL_SKIP_UNSUPPORTED, otl_thread.key,
            otl_thread.no_slot, 0, 0, 0, 0);
  otl_thread.key = 0;
  otl_thread.pool = NULL;
  otl_thread.samples = NULL;
  return s;
}

void dxmt_otl_samples_done(struct otl_samples *s, uint64_t cmdbuf, int done) {
  struct otl_pool *p = s->pool;
  s->cmdbuf = [(id<MTLCommandBuffer>)cmdbuf retain];
  s->done = done;
  s->next = NULL;
  pthread_mutex_lock(&p->mutex);
  *p->done_end = s;
  p->done_end = &s->next;
  pthread_cond_signal(&p->turn);
  pthread_mutex_unlock(&p->mutex);
}

/* The first of four samples of the encoder about to begin, and its sample buffer; nil for none. */
static id<MTLCounterSampleBuffer> otl_sample(uint32_t type, NSUInteger *first) {
  struct otl_pool *p = otl_thread.pool;
  if (!p) {
    otl_thread.no_slot++;
    return nil;
  }
  struct otl_samples *s = otl_thread.samples;
  if (!s && !otl_thread.no_slot) {
    pthread_mutex_lock(&p->mutex);
    s = p->free;
    if (s) {
      p->free = s->next;
      p->out++;
    }
    pthread_mutex_unlock(&p->mutex);
    if (s) {
      s->key = otl_thread.key;
      s->encoders = s->skipped = 0;
      otl_thread.samples = s;
    }
  }
  if (!s) {
    otl_thread.no_slot++;
    return nil;
  }
  if (s->encoders == OTL_ENCODERS) {
    s->skipped++;
    return nil;
  }
  *first = s->encoders * 4;
  s->index[s->encoders++] = (otl_thread.index & 0xffff) | type << 16;
  return s->buffer;
}
