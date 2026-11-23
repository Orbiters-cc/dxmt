/*
 * samples_test.m - the GPU times of encoders (src/winemetal/unix/otl_samples.h) as DXMT's threads use them, on this Mac's Metal.
 * Build and run: run-samples-test.sh (ThreadSanitizer, then without it).
 *
 * Several queues at once, each with a thread that encodes and one that sees command buffers done, as DXMT's. Queues end and are
 * made again with work in flight, some command buffers fail, some have more encoders than fit, and for a while more are in
 * flight than there are sample buffers. What was written is then checked by run-samples-test.sh.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define DXMT_NATIVE 1
#define OTL_IMPLEMENTATION
#include "../../src/winemetal/unix/otl_samples.h"

#define RING 32
static id<MTLDevice> device;
static id<MTLTexture> target;

/* A command buffer that failed, as far as what waits for one can tell (a GPU fault is not made on purpose on a Mac in use). */
@interface Failed : NSObject
@end
@implementation Failed
- (MTLCommandBufferStatus)status { return MTLCommandBufferStatusError; }
- (void)waitUntilCompleted {}
@end

struct chunk { uint64_t id; id<MTLCommandBuffer> cmdbuf; struct otl_samples *samples; };
struct queue {
  uint64_t id;
  struct otl_pool *pool;
  id<MTLCommandQueue> queue;
  struct chunk chunks[RING];
  _Atomic(uint64_t) encoded, finished;
  _Atomic(int) stop;
  unsigned seed;
};

static void encode(struct queue *q, uint64_t at) {
  @autoreleasepool {
    struct chunk *c = &q->chunks[at % RING];
    c->id = at;
    c->cmdbuf = [[q->queue commandBuffer] retain];
    const int recorded = at % 3 != 2;
    const unsigned kind = rand_r(&q->seed) % 40, encoders = kind == 0 ? OTL_ENCODERS + 20 : 1 + kind % 7;
    dxmt_otl_encoding(q->id, at, recorded, q->pool);
    for (unsigned e = 0; e < encoders; e++) {
      NSUInteger first = 0;
      const uint32_t type = e % 3;
      id<MTLCounterSampleBuffer> samples = nil;
      if (otl_thread.key) {
        otl_thread.index++;
        otl_put(otl_now(), type == 0 ? OTL_D_PASS : type == 1 ? OTL_D_COMPUTE : OTL_D_BLIT, otl_thread.index & 0xffff,
                otl_thread.key, 0, 0, 0, 0, 0);
        samples = otl_sample(type, &first);
      }
      if (type == 0) {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = target;
        d.colorAttachments[0].loadAction = MTLLoadActionClear;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (samples) {
          d.sampleBufferAttachments[0].sampleBuffer = samples;
          d.sampleBufferAttachments[0].startOfVertexSampleIndex = first;
          d.sampleBufferAttachments[0].endOfVertexSampleIndex = first + 1;
          d.sampleBufferAttachments[0].startOfFragmentSampleIndex = first + 2;
          d.sampleBufferAttachments[0].endOfFragmentSampleIndex = first + 3;
        }
        [[c->cmdbuf renderCommandEncoderWithDescriptor:d] endEncoding];
      } else if (type == 1) {
        MTLComputePassDescriptor *d = [MTLComputePassDescriptor computePassDescriptor];
        if (samples) {
          d.sampleBufferAttachments[0].sampleBuffer = samples;
          d.sampleBufferAttachments[0].startOfEncoderSampleIndex = first;
          d.sampleBufferAttachments[0].endOfEncoderSampleIndex = first + 1;
        }
        id<MTLComputeCommandEncoder> enc = [c->cmdbuf computeCommandEncoderWithDescriptor:d];
        [enc endEncoding];
      } else {
        MTLBlitPassDescriptor *d = [MTLBlitPassDescriptor blitPassDescriptor];
        if (samples) {
          d.sampleBufferAttachments[0].sampleBuffer = samples;
          d.sampleBufferAttachments[0].startOfEncoderSampleIndex = first;
          d.sampleBufferAttachments[0].endOfEncoderSampleIndex = first + 1;
        }
        id<MTLBlitCommandEncoder> enc = [c->cmdbuf blitCommandEncoderWithDescriptor:d];
        [enc endEncoding];
      }
    }
    c->samples = dxmt_otl_encoded();
    [c->cmdbuf commit];
  }
}

static void *encoder(void *p) {
  struct queue *q = p;
  while (!atomic_load(&q->stop)) {
    uint64_t next = atomic_load(&q->encoded) + 1;
    if (next - atomic_load(&q->finished) >= RING) { usleep(50); continue; }
    encode(q, next);
    atomic_store_explicit(&q->encoded, next, memory_order_release);
  }
  return NULL;
}

static void *finisher(void *p) {
  struct queue *q = p;
  while (!atomic_load(&q->stop)) {
    uint64_t next = atomic_load(&q->finished) + 1;
    if (next > atomic_load_explicit(&q->encoded, memory_order_acquire)) { usleep(50); continue; }
    struct chunk *c = &q->chunks[next % RING];
    [c->cmdbuf waitUntilCompleted];
    const int failed = c->id % 17 == 0;
    Failed *f = failed ? [[Failed alloc] init] : nil;
    if (c->samples) dxmt_otl_samples_done(c->samples, failed ? (uint64_t)f : (uint64_t)c->cmdbuf, 1);
    [f release];
    otl_put(otl_now(), OTL_D_DONE, (uint32_t)c->cmdbuf.status, c->id, (uint64_t)(c->cmdbuf.GPUStartTime * 1e9),
            (uint64_t)(c->cmdbuf.GPUEndTime * 1e9), 0, 0, q->id);
    c->samples = NULL;
    [c->cmdbuf release];
    c->cmdbuf = nil;
    atomic_store(&q->finished, next);
  }
  return NULL;
}

static void *life(void *p) {
  const unsigned n = (unsigned)(uintptr_t)p;
  for (unsigned round = 0; round < 6; round++) {
    struct queue *q = calloc(1, sizeof(*q));
    q->seed = n * 100 + round;
    q->queue = [device newCommandQueueWithMaxCommandBufferCount:RING * 2];
    q->id = dxmt_otl_queue((uint64_t)device, &q->pool);
    pthread_t e, f;
    pthread_create(&e, NULL, encoder, q);
    pthread_create(&f, NULL, finisher, q);
    usleep(400000 + 100000 * n);
    atomic_store(&q->stop, 1); /* with work in flight, as DXMT's queue ends */
    pthread_join(e, NULL);
    pthread_join(f, NULL);
    for (int i = 0; i < RING; i++) {
      struct chunk *c = &q->chunks[i];
      if (c->samples) dxmt_otl_samples_done(c->samples, (uint64_t)c->cmdbuf, 0);
      [c->cmdbuf release];
    }
    dxmt_otl_queue_ended(q->id, q->pool);
    [q->queue release];
    free(q);
  }
  return NULL;
}

int main(void) {
  @autoreleasepool {
    otl_init("dxmt");
    if (!otl_on) { printf("off\n"); return 1; }
    device = MTLCreateSystemDefaultDevice();
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    target = [device newTextureWithDescriptor:td];
    pthread_t t[4];
    for (uintptr_t i = 0; i < 4; i++) pthread_create(&t[i], NULL, life, (void *)i);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    sleep(5); /* the pools go by themselves */
  }
  exit(0);
}
