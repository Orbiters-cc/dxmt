/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Marc-Aurel Zent for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxmt_command_queue.hpp"
#include "Metal.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <atomic>

#define ASYNC_ENCODING 1

namespace dxmt {

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue->AllocateCommandData(size, alignment);
}

CommandQueue::CommandQueue(WMT::Device device) :
    encodeThread([this]() { this->EncodingThread(); }),
    finishThread([this]() { this->WaitForFinishThread(); }),
    device(device),
    commandQueue(device.newCommandQueue(kCommandChunkCount)),
    shared_event_listener(SharedEventListener_create()),
    event_listener_thread([this]() { SharedEventListener_start(this->shared_event_listener); }),
    staging_allocator({
        device, WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                    WMTResourceStorageModeManaged, false
    }),
    copy_temp_allocator({device, WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}),
    argbuf_allocator({
        device,
        WMTResourceHazardTrackingModeUntracked | WMTResourceCPUCacheModeWriteCombined | WMTResourceStorageModeShared
    }),
    cpu_command_allocator({}),
    reftracker_storage_allocator({}),
    cmd_library(device),
    argument_encoding_ctx(*this, device, cmd_library),
    initializer(device),
    counter_pool(device) {
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.queue = this;
    chunk.reset();
  };
  event = device.newSharedEvent();
  otl_init("dxmt");
  otl_id_ = dxmt_otl_queue(device.handle, &otl_pool_);

  std::string env = env::getEnvVar("DXMT_CAPTURE_FRAME");

  if (!env.empty()) {
    try {
      capture_state.scheduleNextFrameCapture(std::stoull(env));
    } catch (const std::invalid_argument &) {
    }
  }
}

CommandQueue::~CommandQueue() {
  TRACE("Destructing command queue");
  stopped.store(true);
  ready_for_encode++;
  ready_for_encode.notify_one();
  ready_for_commit++;
  ready_for_commit.notify_one();
  SharedEventListener_destroy(shared_event_listener);
  encodeThread.join();
  finishThread.join();
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    if (chunk.otl_samples_)
      dxmt_otl_samples_done(chunk.otl_samples_, chunk.attached_cmdbuf.handle, 0);
    chunk.reset();
  };
  dxmt_otl_queue_ended(otl_id_, otl_pool_);
  event_listener_thread.join();
  TRACE("Destructed command queue");
}

void
CommandQueue::CommitCurrentChunk() {
  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % kCommandChunkCount];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.frame_ = frame_count;
  chunk.resource_initializer_event_id = initializer.flushToWait();
  auto& statistics = CurrentFrameStatistics();
  statistics.command_buffer_count++;
  if (otl_active()) {
    chunk.otl_recorded_ = otl_passes && otl_group_ % otl_passes == 0;
    otl_put(otl_now(), OTL_D_READY, chunk.otl_recorded_, otl_id_, chunk_id, chunk.resource_initializer_event_id, otl_group_,
            0, 0);
  }
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();

  auto t0 = clock::now();
  chunk_ongoing.wait(kCommandChunkCount - 1, std::memory_order_acquire);
  chunk_ongoing.fetch_add(1, std::memory_order_relaxed);
  auto t1 = clock::now();
  statistics.commit_interval += (t1 - t0);

#else
  CommitChunkInternal(chunk, ready_for_encode.fetch_add(1, std::memory_order_relaxed));
#endif

  cpu_command_allocator.free_blocks(cpu_coherent.signaledValue());
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk, uint64_t seq) {

  auto pool = WMT::MakeAutoreleasePool();
  const bool otl = otl_active();
  const uint64_t otl_woken = otl ? otl_now() : 0;
  if (otl) {
    dxmt_otl_take_pipeline_wait();
    dxmt_otl_encoding(otl_id_, chunk.chunk_id, chunk.otl_recorded_, otl_pool_);
  }

  switch (capture_state.getNextAction(chunk.frame_)) {
  case CaptureState::NextAction::StartCapture: {
    WMTCaptureInfo info;
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    info.capture_object = device;
    info.destination = WMTCaptureDestinationGPUTraceDocument;
    char filename[1024];
    std::time_t now;
    std::time(&now);
    std::strftime(filename, 1024, "_%H'%M'%S_%m-%d-%y.gputrace", std::localtime(&now));
    auto fileUrl = env::getUnixPath(env::getExeBaseName() + "_F." + std::to_string(chunk.frame_) + filename);
    WARN("A new capture will be saved to ", fileUrl);
    info.output_url.set(fileUrl.c_str());

    capture_mgr.startCapture(info);
    break;
  }
  case CaptureState::NextAction::StopCapture: {
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    capture_mgr.stopCapture();
    break;
  }
  case CaptureState::NextAction::Nothing: {
    if (capture_state.shouldCaptureNextFrame()) {
      capture_state.scheduleNextFrameCapture(chunk.frame_ + 1);
    }
    break;
  }
  }

  auto cmdbuf = commandQueue.commandBuffer();
  chunk.attached_cmdbuf = cmdbuf;
  if (chunk.resource_initializer_event_id) {
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
  }
  chunk.encode(chunk.attached_cmdbuf, this->argument_encoding_ctx);
  const uint64_t otl_flushed = otl ? otl_now() : 0;
  if (otl) {
    chunk.otl_samples_ = dxmt_otl_encoded();
    chunk.otl_committed_ = otl_flushed; // the GPU may start before commit() returns
  }
  cmdbuf.commit();
  if (otl) {
    const uint64_t waited = dxmt_otl_take_pipeline_wait();
    otl_put(otl_now(), OTL_D_ENCODE, 0, chunk.chunk_id, otl_woken, chunk.otl_executed_, otl_flushed, waited, otl_id_);
    otl_hist(OTL_H_A, otl_flushed - otl_woken);
    otl_hist(OTL_H_D, waited);
  }

  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_one();
}

uint32_t
CommandQueue::EncodingThread() {
#if ASYNC_ENCODING
  env::setThreadName("dxmt-encode-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_encode.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    // perform...
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    CommitChunkInternal(chunk, internal_seq);
    internal_seq++;
  }
  TRACE("encoder thread gracefully terminates");
#endif
  return 0;
}

uint32_t
CommandQueue::WaitForFinishThread() {
  env::setThreadName("dxmt-finish-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_commit.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    if (chunk.attached_cmdbuf.status() <= WMTCommandBufferStatusScheduled) {
      chunk.attached_cmdbuf.waitUntilCompleted();
    }
    // What the timeline reads of the chunk: read once everybody who waits for it was told.
    const uint64_t otl_seen = otl_active() ? otl_now() : 0;
    WMT::Reference<WMT::CommandBuffer> otl_cmdbuf;
    uint64_t otl_chunk = 0, otl_committed = 0;
    struct otl_samples *otl_samples = nullptr;
    if (otl_seen && chunk.otl_committed_) {
      otl_cmdbuf = WMT::Reference<WMT::CommandBuffer>(chunk.attached_cmdbuf);
      otl_chunk = chunk.chunk_id;
      otl_committed = chunk.otl_committed_;
      chunk.otl_committed_ = 0;
    }
    std::swap(otl_samples, chunk.otl_samples_);
    if (chunk.attached_cmdbuf.status() == WMTCommandBufferStatusError) {
      ERR("Device error at frame ", chunk.frame_, ": ", chunk.attached_cmdbuf.error().description().getUTF8String());
    }
    if (auto logs = chunk.attached_cmdbuf.logs()) {
      for (auto &log : logs.elements()) {
        ERR("Frame ", chunk.frame_, ": ", log.description().getUTF8String());
      }
    }

    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    chunk.reset();
    cpu_coherent.signal(internal_seq);
    chunk_ongoing.fetch_sub(1, std::memory_order_release);
    chunk_ongoing.notify_one();

    staging_allocator.free_blocks(internal_seq);
    copy_temp_allocator.free_blocks(internal_seq);
    argbuf_allocator.free_blocks(internal_seq);

    if (otl_samples)
      dxmt_otl_samples_done(otl_samples, otl_cmdbuf.handle, 1);
    if (otl_committed) {
      const uint64_t start = MTLCommandBuffer_property(otl_cmdbuf.handle, WMTCommandBufferPropertyGPUStartTime);
      const uint64_t end = MTLCommandBuffer_property(otl_cmdbuf.handle, WMTCommandBufferPropertyGPUEndTime);
      otl_put(otl_seen, OTL_D_DONE, uint32_t(otl_cmdbuf.status()), otl_chunk, start, end,
              MTLCommandBuffer_property(otl_cmdbuf.handle, WMTCommandBufferPropertyKernelStartTime),
              MTLCommandBuffer_property(otl_cmdbuf.handle, WMTCommandBufferPropertyKernelEndTime), otl_id_);
      if (end > start)
        otl_hist(OTL_H_C, end - start);
      if (start > otl_committed)
        otl_hist(OTL_H_B, start - otl_committed);
    }

    internal_seq++;
  }
  TRACE("finishing thread gracefully terminates");
  return 0;
}

void CommandQueue::Retain(uint64_t seq, Allocation* allocation) {
  auto &chunk = chunks[seq % kCommandChunkCount];
  auto &tracker = chunk.ref_tracker;
  constexpr size_t block_size = decltype(reftracker_storage_allocator)::block_size;
  while (unlikely(!tracker.track(allocation))) {
    auto [temp_buffer, _] = reftracker_storage_allocator.allocate(seq, cpu_coherent.signaledValue(), block_size, 1);
    tracker.addStorage(temp_buffer.ptr, block_size);
  }
};

} // namespace dxmt
