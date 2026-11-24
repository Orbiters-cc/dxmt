#include "config/config.hpp"
#include "d3d11_fence.hpp"
#include "d3d11_private.h"
#include "d3d11_query.hpp"
#include "d3d11_query_rescue.hpp"
#include "dxmt_command_queue.hpp"
#include "d3d11_context_impl.cpp"
#include "dxmt_context.hpp"
#include "dxmt_staging.hpp"
#include "dxmt_otl.h"
#include "util_env.hpp"

namespace dxmt {
struct ContextInternalState {
  using device_mutex_t = d3d11_device_mutex;
  CommandQueue &cmd_queue;
  bool has_dirty_op_since_last_event = false;
};


template<typename Object> Rc<Object> forward_rc(Rc<Object>& obj) {
  return std::move(obj);
}

using ImmediateContextBase = MTLD3D11DeviceContextImplBase<ContextInternalState>;

template <>
template <CommandWithContext<ArgumentEncodingContext> cmd>
void
ImmediateContextBase::EmitST(cmd &&fn) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  chk->emitcc(std::forward<cmd>(fn));
}

template <>
template <CommandWithContext<ArgumentEncodingContext> cmd>
void
ImmediateContextBase::EmitOP(cmd &&fn) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  chk->emitcc(std::forward<cmd>(fn));
  ctx_state.has_dirty_op_since_last_event = true;
}

template <>
template <typename T>
moveonly_list<T>
ImmediateContextBase::AllocateCommandData(size_t n) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  return moveonly_list<T>((T *)chk->allocate_cpu_heap(sizeof(T) * n, alignof(T)), n);
}

template <>
std::tuple<WMT::Buffer, uint64_t>
ImmediateContextBase::AllocateStagingBuffer(size_t size, size_t alignment) {
  return ctx_state.cmd_queue.AllocateStagingBuffer(size, alignment);
}

template <>
void
ImmediateContextBase::UseCopyDestination(Rc<StagingResource> &staging) {
  staging->useCopyDestination(ctx_state.cmd_queue.CurrentSeqId());
}

template <>
void
ImmediateContextBase::UseCopySource(Rc<StagingResource> &staging) {
  staging->useCopySource(ctx_state.cmd_queue.CurrentSeqId());
}

template <>
std::pair<BufferAllocation *, uint32_t>
ImmediateContextBase::GetDynamicBufferAllocation(Rc<DynamicBuffer> &dynamic) {
  return {dynamic->immediateName().ptr(), dynamic->immediateSuballocation()};
}

class MTLD3D11ImmediateContext : public ImmediateContextBase {
public:
  MTLD3D11ImmediateContext(MTLD3D11Device *pDevice, CommandQueue &cmd_queue) :
      ImmediateContextBase(pDevice, ctx_state, pDevice->mutex),
      cmd_queue(cmd_queue),
      ctx_state({cmd_queue}),
      d3dmt_(this, mutex) {
        ignore_map_flag_no_wait_ = Config::getInstance().getOption<bool>("d3d11.ignoreMapFlagNoWait", false);
      }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(ID3D11Multithread)) {
      *ppvObject = ref(&d3dmt_);
      return S_OK;
    }

    return ImmediateContextBase::QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE
  AddRef() override {
    uint32_t refCount = this->refcount++;
    if (unlikely(!refCount))
      this->m_parent->AddRef();

    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE
  Release() override {
    uint32_t refCount = --this->refcount;
    D3D11_ASSERT(refCount != ~0u && "try to release a 0 reference object");
    if (unlikely(!refCount))
      this->m_parent->Release();

    return refCount;
  }

  void *
  MapDynamicBuffer(Rc<DynamicBuffer> &dynamic, uint64_t current_seq_id, uint64_t coherent_seq_id) {
    if (auto next_sub = dynamic->nextSuballocation()) {
      EmitST([allocation = dynamic->immediateName(), next_sub](ArgumentEncodingContext &enc) mutable {
        allocation->useSuballocation(next_sub);
      });
    } else {
      dynamic->updateImmediateName(current_seq_id, dynamic->allocate(coherent_seq_id), 0, false);
      EmitST([allocation = dynamic->immediateName(),
              buffer = Rc(dynamic->buffer)](ArgumentEncodingContext &enc) mutable {
        allocation->useSuballocation(0);
        auto _ = buffer->rename(forward_rc(allocation));
      });
    }

    return dynamic->immediateMappedMemory();
  }

  HRESULT
  STDMETHODCALLTYPE
  Map(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
      D3D11_MAPPED_SUBRESOURCE *pMappedResource) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    if (unlikely(!pResource))
      return E_INVALIDARG;
    UINT buffer_length = 0, &row_pitch = buffer_length;
    UINT bind_flag = 0, &depth_pitch = bind_flag;
    auto current_seq_id = cmd_queue.CurrentSeqId();
    auto coherent_seq_id = cmd_queue.CoherentSeqId();
    if (auto dynamic = GetDynamicBuffer(pResource, &buffer_length, &bind_flag)) {
      if (!pMappedResource)
        return E_INVALIDARG;
      switch (MapType) {
      case D3D11_MAP_READ:
      case D3D11_MAP_WRITE:
      case D3D11_MAP_READ_WRITE:
        return E_INVALIDARG;
      case D3D11_MAP_WRITE_DISCARD: {
        if (bind_flag & D3D11_BIND_VERTEX_BUFFER) {
          state_.InputAssembler.VertexBuffers.set_dirty();
        }
        if (bind_flag & D3D11_BIND_CONSTANT_BUFFER) {
          for (auto &stage : state_.ShaderStages) {
            stage.ConstantBuffers.set_dirty();
          }
        }
        if (bind_flag & D3D11_BIND_SHADER_RESOURCE) {
          for (auto &stage : state_.ShaderStages) {
            stage.SRVs.set_dirty();
          }
        }

        pMappedResource->pData = MapDynamicBuffer(dynamic, current_seq_id, coherent_seq_id);
        pMappedResource->RowPitch = buffer_length;
        pMappedResource->DepthPitch = buffer_length;
        break;
      }
      case D3D11_MAP_WRITE_NO_OVERWRITE: {
        pMappedResource->pData = dynamic->immediateMappedMemory();
        pMappedResource->RowPitch = buffer_length;
        pMappedResource->DepthPitch = buffer_length;
        break;
      }
      }
      return S_OK;
    }
    if (auto dynamic = GetDynamicTexture(pResource, Subresource, &row_pitch, &depth_pitch)) {
      if (!pMappedResource)
        return E_INVALIDARG;
      switch (MapType) {
      case D3D11_MAP_READ:
      case D3D11_MAP_WRITE:
      case D3D11_MAP_READ_WRITE:
      case D3D11_MAP_WRITE_NO_OVERWRITE:
        return E_INVALIDARG;
      case D3D11_MAP_WRITE_DISCARD: {
        for (auto &stage : state_.ShaderStages) {
          stage.SRVs.set_dirty();
        }

        pMappedResource->pData = MapDynamicBuffer(dynamic, current_seq_id, coherent_seq_id);
        pMappedResource->RowPitch = row_pitch;
        pMappedResource->DepthPitch = depth_pitch;
        break;
      }
      }
      return S_OK;
    }
    if (auto dynamic = GetDynamicLinearTexture(pResource, &row_pitch, &depth_pitch)) {
      if (!pMappedResource)
        return E_INVALIDARG;
      switch (MapType) {
      case D3D11_MAP_READ:
      case D3D11_MAP_WRITE:
      case D3D11_MAP_READ_WRITE:
        return E_INVALIDARG;
      case D3D11_MAP_WRITE_DISCARD: {
        for (auto &stage : state_.ShaderStages) {
          stage.SRVs.set_dirty();
        }

        dynamic->updateImmediateName(current_seq_id, dynamic->allocate(coherent_seq_id), false);
        EmitST([allocation = dynamic->immediateName(),
              texture = Rc(dynamic->texture)](ArgumentEncodingContext &enc) mutable {
          auto _ = texture->rename(forward_rc(allocation));
        });

        pMappedResource->pData = dynamic->mappedMemory();
        pMappedResource->RowPitch = row_pitch;
        pMappedResource->DepthPitch = depth_pitch;
        break;
      }
      case D3D11_MAP_WRITE_NO_OVERWRITE: {
        pMappedResource->pData = dynamic->mappedMemory();
        pMappedResource->RowPitch = row_pitch;
        pMappedResource->DepthPitch = depth_pitch;
        break;
      }
      }
      return S_OK;
    }
    if (auto staging = GetStagingResource(pResource, Subresource)) {
      if (!pMappedResource)
        return E_INVALIDARG;

      if (MapType > 3 || MapType == 0)
        return E_INVALIDARG;

      if (ignore_map_flag_no_wait_)
        MapFlags &= ~D3D11_MAP_FLAG_DO_NOT_WAIT;

      while (true) {
        auto result = staging->tryMap(coherent_seq_id, MapType & D3D11_MAP_READ, MapType & D3D11_MAP_WRITE);
        if (result == StagingMapResult::Mapped)
          return E_FAIL;
        if (result == StagingMapResult::Renamable) {
          // when write to a buffer that is gpu-readonly
          auto next_name = staging->allocate(coherent_seq_id);
          // can't guarantee a full overwrite
          std::memcpy(staging->mappedMemory(next_name), staging->mappedImmediateMemory(), staging->length);
          staging->updateImmediateName(current_seq_id, next_name);
          EmitST([staging, next_name](ArgumentEncodingContext &enc) mutable { 
            auto _ = staging->buffer()->rename(staging->allocation(next_name));
          });
          result = StagingMapResult::Mappable;
        }
        if (result == StagingMapResult::Mappable) {
          TRACE("staging map ready");
          pMappedResource->pData = staging->mappedImmediateMemory();
          pMappedResource->RowPitch = staging->bytesPerRow;
          pMappedResource->DepthPitch = staging->bytesPerImage;
          return S_OK;
        }
        if (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT) {
          return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        // even it's in a while loop
        // only the first flush will have effect
        // and the following calls are essentially no-op
        Flush();
        TRACE("staging map block");
        auto& statistics = cmd_queue.CurrentFrameStatistics();
        auto t0 = clock::now();
        cmd_queue.WaitCPUFence(coherent_seq_id + uint64_t(result));
        auto t1 = clock::now();
        statistics.sync_count++;
        statistics.sync_interval += (t1 - t0);
        current_seq_id = cmd_queue.CurrentSeqId();
        coherent_seq_id = cmd_queue.CoherentSeqId();
      };
    };
    if (pMappedResource == nullptr) {
      UNIMPLEMENTED("map-on-default: map");
    }
    return E_INVALIDARG;
  }

  void
  STDMETHODCALLTYPE
  Unmap(ID3D11Resource *pResource, UINT Subresource) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    if (unlikely(!pResource))
      return;
    UINT row_pitch = 0;
    UINT depth_pitch = 0;
    if (auto staging = GetStagingResource(pResource, Subresource)) {
      staging->unmap();
    };
    if (auto dynamic = GetDynamicTexture(pResource, Subresource, &row_pitch, &depth_pitch)) {
      BlitObject texture(device, pResource);
      UpdateTexture(TextureUpdateCommand(texture, Subresource, nullptr),
                    dynamic->buffer, row_pitch, depth_pitch);
    }
  }

  void
  STDMETHODCALLTYPE
  Begin(ID3D11Asynchronous *pAsync) override {
    if (unlikely(!pAsync))
      return;

    std::lock_guard<d3d11_device_mutex> lock(mutex);

    // in theory pAsync could be any of them: { Query, Predicate, Counter }.
    // However `Predicate` and `Counter` are not supported at all
    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
    case D3D11_QUERY_TIMESTAMP:
    case D3D11_QUERY_EVENT:
      break;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      if (auto query = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->Begin())
        EmitST([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
          enc.beginVisibilityResultQuery(std::move(query));
        });
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      // ignore
      break;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      break;
    }
  }

  // See Begin()
  void
  STDMETHODCALLTYPE
  End(ID3D11Asynchronous *pAsync) override {
    if (unlikely(!pAsync))
      return;

    std::lock_guard<d3d11_device_mutex> lock(mutex);

    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
    case D3D11_QUERY_EVENT: {
      if (ctx_state.has_dirty_op_since_last_event) {
        auto event_id = cmd_queue.GetNextEventSeqId();
        static_cast<MTLD3D11EventQuery *>(pAsync)->Issue(event_id);
        InvalidateCurrentPass(true);
        EmitOP([event_id](ArgumentEncodingContext &enc) mutable {
          enc.signalEvent(event_id);
        });
        promote_flush = true;
        ctx_state.has_dirty_op_since_last_event = false;
      } else {
        static_cast<MTLD3D11EventQuery *>(pAsync)->Issue(cmd_queue.GetCurrentEventSeqId());
      }
      break;
    }
    case D3D11_QUERY_TIMESTAMP: {
      // The sample is taken inside the command buffer: the result is there once that buffer completes, whenever it is
      // committed. No flush here: Unreal Engine ends hundreds of timestamps per frame (GPU stats), and a command buffer
      // for each kept the queue's 32 chunks full (THE FINALS: about 150 command buffers per frame). A GetData without
      // D3D11_ASYNC_GETDATA_DONOTFLUSH still flushes while the result is not there.
      if (auto query = static_cast<MTLD3D11TimestampQuery *>(pAsync)->End()) {
        InvalidateCurrentPass(true);
        EmitOP([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
          enc.sampleTimestamp(std::move(query));
        });
      }
      break;
    }
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      if (auto query = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->End())
        EmitST([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
          enc.endVisibilityResultQuery(std::move(query));
        });
      promote_flush = true;
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      // ignore
      break;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      break;
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT GetDataFlags) override {
    if (!pAsync || (DataSize && !pData))
      return E_INVALIDARG;

    // Allow dataSize to be zero
    if (DataSize && DataSize != pAsync->GetDataSize())
      return E_INVALIDARG;

    HRESULT hr = S_FALSE;

    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_EVENT:
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      switch (static_cast<MTLD3D11EventQuery *>(pAsync)->CheckEventState(cmd_queue.SignaledEventSeqId())) {
      case EventState::Pending:
        break;
      case EventState::Stall:
        GetDataFlags &= ~D3D11_ASYNC_GETDATA_DONOTFLUSH;
        break;
      case EventState::Signaled:
        hr = S_OK;
        break;
      case EventState::Invalid:
        return DXGI_ERROR_INVALID_CALL;
      }
      break;
    }
    default:
      break;
    }
    switch (desc.Query) {
    case D3D11_QUERY_EVENT: {
      if (pData)
        *static_cast<BOOL *>(pData) = (hr == S_OK);
      break;
    }
    case D3D11_QUERY_OCCLUSION: {
      uint64_t null_data;
      uint64_t *data_ptr = pData ? (uint64_t *)pData : &null_data;
      hr = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      BOOL null_data;
      BOOL *data_ptr = pData ? (BOOL *)pData : &null_data;
      hr = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_TIMESTAMP: {
      uint64_t null_data;
      uint64_t *data_ptr = pData ? (uint64_t *)pData : &null_data;
      hr = static_cast<MTLD3D11TimestampQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      if (pData) {
        (*static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT *>(pData)) = {1'000'000'000, FALSE};
      }
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      if (pData) {
        (*static_cast<D3D11_QUERY_DATA_PIPELINE_STATISTICS *>(pData)) = {};
      }
      return S_OK;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      return E_FAIL;
    }
    if (hr == S_FALSE && (GetDataFlags & D3D11_ASYNC_GETDATA_DONOTFLUSH) == 0) {
      cmd_queue.CurrentFrameStatistics().event_stall++;
      Flush();
    }
    return hr;
  }

  void
  STDMETHODCALLTYPE
  Flush() override {
    Flush1(D3D11_CONTEXT_TYPE_ALL, NULL);
  }

  void
  STDMETHODCALLTYPE
  Flush1(D3D11_CONTEXT_TYPE Type, HANDLE hEvent) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    if (ctx_state.has_dirty_op_since_last_event || promote_flush) {
      InvalidateCurrentPass(true);
      Commit();
    }
    if (hEvent) {
      auto shared_event_listener = cmd_queue.GetSharedEventListener();
      MTLSharedEvent_setWin32EventAtValue(
          cmd_queue.event, shared_event_listener, hEvent, cmd_queue.GetCurrentEventSeqId()
      );
    }
  }

  void PrepareFlush() override {
    InvalidateCurrentPass(true);
  }

  void
  STDMETHODCALLTYPE
  ExecuteCommandList(ID3D11CommandList *pCommandList, BOOL RestoreContextState) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    ResetEncodingContextState();

    Com<MTLD3D11CommandList, false> cmdlist = static_cast<MTLD3D11CommandList *>(pCommandList);
    auto seq_id = ctx_state.cmd_queue.CurrentSeqId();

    promote_flush = cmdlist->promote_flush;

    auto query_list = AllocateCommandData<Rc<VisibilityResultQuery>>(cmdlist->visibility_query_count);
    for (const auto &[query, index] : cmdlist->issued_visibility_query) {
      query_list[index] = new VisibilityResultQuery();
      query->DoDeferredQuery(query_list[index].ptr());
    }

    for (const auto &query : cmdlist->issued_event_query) {
      End(query.ptr());
    }

    for (const auto &query : cmdlist->issued_timestamp_query) {
      End(query.ptr());
    }

    for (const auto &staging : cmdlist->read_staging_resources) {
      staging->useCopySource(seq_id);
    }

    for (const auto &staging : cmdlist->written_staging_resources) {
      staging->useCopyDestination(seq_id);
    }

    for (const auto &used_dynamic : cmdlist->used_dynamic_buffers) {
      if (!used_dynamic.latest)
        continue;
      used_dynamic.buffer->updateImmediateName(seq_id, Rc(used_dynamic.allocation), used_dynamic.suballocation, true);
    }

    for (const auto &used_dynamic : cmdlist->used_dynamic_lineartextures) {
      if (!used_dynamic.latest)
        continue;
      used_dynamic.texture->updateImmediateName(seq_id, Rc(used_dynamic.allocation), true);
    }

    EmitOP([cmdlist = std::move(cmdlist), query_list = std::move(query_list)](ArgumentEncodingContext &enc) {
      // Finished command list should clean up the encoding context
      enc.pushDeferredVisibilityQueries(query_list.data());
      cmdlist->Execute(enc);
      enc.popDeferredVisibilityQueries();
    });

    if (RestoreContextState)
      RestoreEncodingContextState();
    else
      ResetD3D11ContextState();
  }

  HRESULT
  STDMETHODCALLTYPE
  FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList **ppCommandList) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    return DXGI_ERROR_INVALID_CALL;
  }

  D3D11_DEVICE_CONTEXT_TYPE
  STDMETHODCALLTYPE
  GetType() override {
    return D3D11_DEVICE_CONTEXT_IMMEDIATE;
  }

  virtual void
  WaitUntilGPUIdle() override {
    uint64_t seq = cmd_queue.CurrentSeqId();
    if(!InvalidateCurrentPass())
      Commit();
    cmd_queue.WaitCPUFence(seq);
  };

  void
  Commit() override {
    promote_flush = false;
    D3D11_ASSERT(cmdbuf_state == CommandBufferState::Idle);
    ctx_state.cmd_queue.CommitCurrentChunk();
    ctx_state.has_dirty_op_since_last_event = false;
  };

  HRESULT STDMETHODCALLTYPE Signal(ID3D11Fence *pFence, UINT64 Value) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto fence = static_cast<MTLD3D11Fence *>(pFence);

    InvalidateCurrentPass();
    EmitOP([event = fence->event, Value](ArgumentEncodingContext &enc) mutable {
      enc.signalEvent(std::move(event), Value);
    });
    if (otl_active())
      otl_put(otl_now(), OTL_D_SIGNAL, 0, cmd_queue.CurrentSeqId(), reinterpret_cast<uintptr_t>(pFence), Value,
              fence->event.handle, cmd_queue.otl_id_, cmd_queue.otl_group_);
    Flush();
    cmd_queue.otl_group_++;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Wait(ID3D11Fence *pFence, UINT64 Value) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto fence = static_cast<MTLD3D11Fence *>(pFence);

    Flush();
    EmitOP([event = fence->event, Value](ArgumentEncodingContext &enc) mutable {
      enc.waitEvent(std::move(event), Value);
    });

    return S_OK;
  }

  UINT
  STDMETHODCALLTYPE
  GetContextFlags() override {
    return 0;
  }

  HRESULT
  AcquireSync(ID3D11Resource *pResource, UINT64 Key, DWORD dwMilliseconds) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto resource = GetResourceCommon(pResource);
    if (!resource->keyedMutex())
      return DXGI_ERROR_INVALID_CALL;
    return resource->keyedMutex()->acquire(
        [&](uint64_t fence) {
          InvalidateCurrentPass(true);
          EmitOP([event = resource->keyedMutex()->sharedEvent(), fence](ArgumentEncodingContext &enc) mutable {
            enc.waitEvent(std::move(event), fence);
          });
          Flush();
        },
        Key, dwMilliseconds
    );
  }
  HRESULT
  ReleaseSync(ID3D11Resource *pResource, UINT64 Key) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto resource = GetResourceCommon(pResource);
    if (!resource->keyedMutex())
      return DXGI_ERROR_INVALID_CALL;
    return resource->keyedMutex()->release(
        [&](uint64_t fence) {
          InvalidateCurrentPass(true);
          EmitOP([event = resource->keyedMutex()->sharedEvent(), fence](ArgumentEncodingContext &enc) mutable {
            enc.signalEvent(std::move(event), fence);
          });
          Flush();
        },
        Key
    );
  }

protected: // (for the two classes below)
  std::vector<Com<MTLD3D11OcclusionQuery>> pending_occlusion_queries;
  CommandQueue &cmd_queue;
  ContextInternalState ctx_state;
  std::atomic<uint32_t> refcount = 0;
  D3D11Multithread d3dmt_;
  bool ignore_map_flag_no_wait_;
};

/**
The immediate context with OBELISK_DXMT_QUERY_RESCUE=once. The class above, untouched, is the one without it: what is different is
overridden here and is here alone (with the queries, MTLD3D11EventQueryOnce, and the command lists', MTLD3D11DeferredContextOnce).

A look at an event or disjoint query with DONOTFLUSH that was not there kEventStallThreshold looks before commits the chunk with its
marker if that chunk was not handed to the encoder, and otherwise does nothing. A look waits for nobody: not for the device's lock,
not for the GPU to be done with a chunk (a full queue), not for the uploads' queue or whoever initializes a resource.

What it could not do for the lock or a full queue is owed: the rescue thread, which sleeps while nothing is, does it as soon as it can,
whether or not anybody looks again. Uploads that are pending are owed too, but hold nothing up: the chunk is committed at once and waits for
them on the GPU, and the rescue thread commits them. That is the one thing of a rescue that may wait (for the GPU, at uploads, which wait for
nothing of the application's), and the rescue thread does it holding no lock but the initializer's own; a context that ends has it done first.

The rescue thread touches the context under the device's lock alone. Where the application did not ask for that lock
(ID3D11Multithread), the context is its own thread's: the rescue thread then commits uploads and nothing else, and a full queue is the
next look's. (An application that takes the protection back while a rescue is owed is not covered: none is owed once its queries are there.)
*/
class MTLD3D11ImmediateContextOnce : public MTLD3D11ImmediateContext {
public:
  MTLD3D11ImmediateContextOnce(MTLD3D11Device *pDevice, CommandQueue &cmd_queue) :
      MTLD3D11ImmediateContext(pDevice, cmd_queue),
      locked_ext_(this, mutex),
      rescue_thread_([this]() { RescueThread(); }) {}

  ~MTLD3D11ImmediateContextOnce() { StopRescues(); }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject && (riid == __uuidof(IMTLD3D11ContextExt) || riid == __uuidof(IMTLD3D11ContextExt1))) {
      *ppvObject = ref(&locked_ext_);
      return S_OK;
    }
    return MTLD3D11ImmediateContext::QueryInterface(riid, ppvObject);
  }

  HRESULT
  STDMETHODCALLTYPE
  GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT GetDataFlags) override {
    if (pAsync && (GetDataFlags & D3D11_ASYNC_GETDATA_DONOTFLUSH) && (!DataSize || (pData && DataSize == pAsync->GetDataSize()))) {
      D3D11_QUERY_DESC desc;
      ((ID3D11Query *)pAsync)->GetDesc(&desc);
      if (desc.Query == D3D11_QUERY_EVENT || desc.Query == D3D11_QUERY_TIMESTAMP_DISJOINT)
        return Look(pAsync, desc.Query, pData);
    }
    // (a look that may flush flushes as ever: under the device's lock, which it waits for)
    return MTLD3D11ImmediateContext::GetData(pAsync, pData, DataSize, GetDataFlags);
  }

  void
  STDMETHODCALLTYPE
  ExecuteCommandList(ID3D11CommandList *pCommandList, BOOL RestoreContextState) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    auto &markers = static_cast<MTLD3D11CommandList *>(pCommandList)->event_markers;
    const bool marked = !markers.empty();
    if (marked) {
      if (ctx_state.has_dirty_op_since_last_event) {
        // what is recorded before the list is kept from being put together with it, as by the End the list's query has without the switch:
        // a render pass joined to one with an occlusion query in it is counted by that query too (dxmt_context.cpp, with the switch and without)
        InvalidateCurrentPass(true);
        EmitOP([event_id = cmd_queue.GetNextEventSeqId()](ArgumentEncodingContext &enc) { enc.signalEvent(event_id); });
      }
      auto events = AllocateCommandData<uint64_t>(markers.size());
      for (size_t i = 0; i < markers.size(); i++) {
        // in their order in the list; a query ended twice in it is signalled by the last
        events[i] = cmd_queue.GetNextEventSeqId();
        markers[i]->Issue(events[i]);
      }
      // the events are of this chunk, as the list is: nothing commits before it is emitted
      EmitST([events = std::move(events)](ArgumentEncodingContext &enc) { enc.pushDeferredEvents(events.data()); });
    }
    MTLD3D11ImmediateContext::ExecuteCommandList(pCommandList, RestoreContextState);
    if (marked)
      EmitST([](ArgumentEncodingContext &enc) { enc.popDeferredEvents(); });
  }

  void
  WaitUntilGPUIdle() override {
    uint64_t seq;
    {
      std::lock_guard<d3d11_device_mutex> lock(mutex);
      seq = cmd_queue.CurrentSeqId();
      if (!InvalidateCurrentPass())
        Commit();
    }
    cmd_queue.WaitCPUFence(seq);
  }

  void
  Commit() override {
    MTLD3D11ImmediateContext::Commit();
    Submitted();
  }

protected:
  enum class Rescue {
    Done,    // the chunk was committed
    Uploads, // the same, and the uploads it waits for are owed
    Covered, // nothing is to be rescued (any more)
    Lock,    // someone else is on the device
    Queue,   // the queue is full
  };

  /* For the timeline: what came of a look's rescue, or (`owed`) of the rescue thread's. */
  virtual void RescueTold(Rescue outcome, uint64_t chunk, uint64_t event_id, uint64_t before, uint64_t after, uint64_t began, bool owed) {}

  /* Every event up to this one is in a chunk that was handed to the encoder: it will be signalled without another call. Not what was
   * reserved (GetCurrentEventSeqId), not what the GPU signalled (SignaledEventSeqId). */
  uint64_t
  SubmittedEventSeqId() {
    return submitted_event_seq_id_.load(std::memory_order_acquire);
  }

  /* Commit() for a rescue: the uploads are known, and nothing is waited for. */
  virtual void
  CommitRescue(uint64_t resource_initializer_event_id) {
    promote_flush = false;
    D3D11_ASSERT(cmdbuf_state == CommandBufferState::Idle);
    ctx_state.cmd_queue.CommitCurrentChunkAfter(resource_initializer_event_id);
    ctx_state.has_dirty_op_since_last_event = false;
    Submitted();
  }

  void
  StopRescues() {
    if (!rescue_thread_.joinable())
      return;
    {
      std::lock_guard<dxmt::mutex> lock(owed_lock_);
      stopping_ = true;
    }
    owed_wake_.notify_one();
    rescue_thread_.join();
  }

private:
  /* (by whoever commits, so under the device's lock, as every End and every other commit is) */
  void
  Submitted() {
    submitted_event_seq_id_.store(cmd_queue.GetCurrentEventSeqId(), std::memory_order_release);
  }

  HRESULT
  Look(ID3D11Asynchronous *pAsync, D3D11_QUERY kind, void *pData) {
    auto query = EventQueryAs<MTLD3D11EventQueryOnce, EventQueryLook>(pAsync, kind);
    uint64_t stalled = 0;
    const auto state = query->Look(cmd_queue.SignaledEventSeqId(), stalled);
    if (state == EventState::Invalid)
      return DXGI_ERROR_INVALID_CALL;
    if (pData) {
      if (kind == D3D11_QUERY_EVENT)
        *static_cast<BOOL *>(pData) = state == EventState::Signaled;
      else
        *static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT *>(pData) = {1'000'000'000, FALSE};
    }
    if (state == EventState::Stall)
      RescueLooked(query, stalled);
    return state == EventState::Signaled ? S_OK : S_FALSE;
  }

  void
  RescueLooked(EventQueryLook *query, uint64_t event_id) {
    const uint64_t began = otl_active() ? otl_now() : 0;
    if (const uint64_t submitted = SubmittedEventSeqId(); event_id <= submitted)
      return RescueTold(Rescue::Covered, 0, event_id, submitted, submitted, began, false);
    // (the query's last End, under the lock every End is made under: what it was before is nobody's any more)
    const auto outcome = TryRescue([query] { return query->StalledAt(); }, began, false);
    if ((outcome == Rescue::Lock || outcome == Rescue::Queue) && mutex.get_protected())
      Owe(event_id, 0);
  }

  /* Commits the current chunk if the marker of `target()` is in it. Waits for nothing. */
  template <typename Target>
  Rescue
  TryRescue(Target &&target, uint64_t began, bool owed) {
    std::unique_lock<d3d11_device_mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      RescueTold(Rescue::Lock, 0, 0, 0, 0, began, owed);
      return Rescue::Lock;
    }
    const uint64_t event_id = target(), before = SubmittedEventSeqId(), chunk = cmd_queue.CurrentSeqId();
    auto outcome = Rescue::Done;
    if (event_id <= before) {
      outcome = Rescue::Covered;
    } else if (cmd_queue.CommitWaits()) {
      outcome = Rescue::Queue;
    } else {
      cmd_queue.CurrentFrameStatistics().event_stall++;
      InvalidateCurrentPass(true, OTL_WHY_GETDATA_STALL);
      // what the chunk uses was created before it was used, so before this
      bool uploads;
      const uint64_t initializer_event_id = cmd_queue.initializer.reserveToWait(uploads);
      CommitRescue(initializer_event_id);
      if (uploads) {
        outcome = Rescue::Uploads;
        Owe(0, initializer_event_id);
      }
    }
    RescueTold(outcome, chunk, event_id, before, SubmittedEventSeqId(), began, owed);
    return outcome;
  }

  /* The chunk with the marker of `event_id`, and the uploads up to `initializer_event_id`, are the rescue thread's to commit (0: none). */
  void
  Owe(uint64_t event_id, uint64_t initializer_event_id) {
    {
      std::lock_guard<dxmt::mutex> lock(owed_lock_);
      owed_event_ = std::max(owed_event_, event_id);
      owed_uploads_ = std::max(owed_uploads_, initializer_event_id);
    }
    owed_wake_.notify_one();
  }

  void
  RescueThread() {
    env::setThreadName("dxmt-rescue-thread");
    std::unique_lock<dxmt::mutex> lock(owed_lock_);
    while (true) {
      owed_wake_.wait(lock, [this] { return stopping_ || owed_event_ || owed_uploads_; });
      if (const uint64_t uploads = owed_uploads_) { // first, and before the end: a chunk of the GPU's waits for them
        owed_uploads_ = 0;
        lock.unlock();
        cmd_queue.initializer.flushThrough(uploads);
        lock.lock();
        continue;
      }
      if (stopping_)
        return;
      const uint64_t event_id = owed_event_;
      lock.unlock();
      auto outcome = Rescue::Covered;
      if (mutex.get_protected())
        outcome = TryRescue([event_id] { return event_id; }, otl_active() ? otl_now() : 0, true);
      lock.lock();
      if (outcome == Rescue::Lock || outcome == Rescue::Queue)
        owed_wake_.wait_for(lock, std::chrono::microseconds(100), [this] { return stopping_ || owed_uploads_; });
      else if (owed_event_ == event_id)
        owed_event_ = 0;
    }
  }

  /* The extension's calls record without the device's lock, which a rescue by another thread relies on. */
  class LockedExt : public IMTLD3D11ContextExt1 {
  public:
    LockedExt(MTLD3D11ImmediateContextOnce *context, d3d11_device_mutex &mutex) : context_(context), mutex_(mutex) {}
    HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, void **ppvObject) final {
      return context_->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() final { return context_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() final { return context_->Release(); }
    void STDMETHODCALLTYPE
    TemporalUpscale(const MTL_TEMPORAL_UPSCALE_D3D11_DESC *pDesc) final {
      std::lock_guard<d3d11_device_mutex> lock(mutex_);
      context_->ext_.TemporalUpscale(pDesc);
    }
    void STDMETHODCALLTYPE
    BeginUAVOverlap() final {
      std::lock_guard<d3d11_device_mutex> lock(mutex_);
      context_->ext_.BeginUAVOverlap();
    }
    void STDMETHODCALLTYPE
    EndUAVOverlap() final {
      std::lock_guard<d3d11_device_mutex> lock(mutex_);
      context_->ext_.EndUAVOverlap();
    }
    HRESULT STDMETHODCALLTYPE
    CheckFeatureSupport(MTL_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) final {
      return context_->ext_.CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
    }

  private:
    MTLD3D11ImmediateContextOnce *context_;
    d3d11_device_mutex &mutex_;
  };

  LockedExt locked_ext_;
  std::atomic<uint64_t> submitted_event_seq_id_ = 0;
  dxmt::mutex owed_lock_; // (a leaf: nothing else is taken or waited for under it)
  dxmt::condition_variable owed_wake_;
  uint64_t owed_event_ = 0;
  uint64_t owed_uploads_ = 0;
  bool stopping_ = false;
  dxmt::thread rescue_thread_; // (the last: what it uses is made)
};

/**
Either of the two, of a device that is made while the timeline is on: what it calls is what they are, the timeline says what came of it.
Nothing of it decides anything. Looks read nothing that Ends write but under the query's own trace lock.
*/
template <typename Base, bool Once>
class TimedImmediateContext : public Base {
  template <typename DataType>
  using Query = std::conditional_t<Once, EventQueryOnceTimed<DataType>, EventQueryTimed<DataType>>;

public:
  using Base::Base;

  ~TimedImmediateContext() {
    if (otl_active())
      Told(4, this->cmd_queue.otl_group_);
  }

  HRESULT
  STDMETHODCALLTYPE
  Map(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource) override {
    OtlCall call(OTL_WHY_MAP_STAGING);
    return Base::Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
  }

  void
  STDMETHODCALLTYPE
  End(ID3D11Asynchronous *pAsync) override {
    if (!pAsync || !otl_active())
      return Base::End(pAsync);
    std::lock_guard<d3d11_device_mutex> lock(this->mutex);
    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    uint32_t kind = 4, why = OTL_WHY_OTHER;
    bool clean = false;
    switch (desc.Query) {
    case D3D11_QUERY_EVENT:
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
      kind = desc.Query != D3D11_QUERY_EVENT;
      why = OTL_WHY_END_EVENT;
      clean = !this->ctx_state.has_dirty_op_since_last_event;
      break;
    case D3D11_QUERY_TIMESTAMP:
      kind = 2;
      why = OTL_WHY_END_TIMESTAMP;
      break;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
      kind = 3;
      why = OTL_WHY_END_OCCLUSION;
      break;
    default:
      break;
    }
    if (kind < 4) {
      std::lock_guard<dxmt::mutex> told(told_lock_);
      told_.ended[kind]++;
      told_.clean += clean;
    }
    OtlCall call(why);
    const uint32_t former = otl_query_end;
    otl_query_end |= clean ? 2 : 0;
    const bool asked = this->promote_flush;
    Base::End(pAsync);
    otl_query_end = former;
    if (this->promote_flush && !asked) {
      this->otl_asked_ = why;
      this->otl_asked_at_ = otl_now();
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT GetDataFlags) override {
    OtlCall call(OTL_WHY_GETDATA);
    D3D11_QUERY_DESC desc;
    if (!pAsync || !otl_active())
      return Base::GetData(pAsync, pData, DataSize, GetDataFlags);
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    if (desc.Query != D3D11_QUERY_EVENT && desc.Query != D3D11_QUERY_TIMESTAMP_DISJOINT)
      return Base::GetData(pAsync, pData, DataSize, GetDataFlags);
    auto &trace = EventQueryAs<Query, EventQueryTraced>(pAsync, desc.Query)->trace;
    const bool quiet = GetDataFlags & D3D11_ASYNC_GETDATA_DONOTFLUSH;
    uint32_t generation, looks;
    uint64_t event_id;
    {
      std::lock_guard<dxmt::mutex> lock(trace.lock);
      generation = trace.generation;
      looks = trace.looks;
      event_id = trace.event;
    }
    if (quiet)
      otl_call = OTL_WHY_GETDATA_STALL;
    Looking here = {&trace, 0};
    Looking *const former = looking;
    looking = quiet ? &here : nullptr;
    const uint64_t began = otl_now();
    const HRESULT hr = Base::GetData(pAsync, pData, DataSize, GetDataFlags);
    const uint64_t ended = otl_now();
    looking = former;
    if (FAILED(hr))
      return hr;
    TraceLook(trace, this->cmd_queue, generation, began, ended, hr);
    const bool late = hr == S_FALSE && looks >= kEventStallThreshold;
    if (!Once && quiet && late && !here.commits) {
      // (nothing was recorded, so that nothing was flushed: said only where the marker is not handed over all the same)
      const uint64_t submitted = submitted_.load(std::memory_order_acquire);
      if (event_id > submitted)
        otl_put(ended, OTL_D_STALL, 4, this->cmd_queue.otl_id_, this->cmd_queue.CurrentSeqId(), event_id, submitted, submitted, 0);
    }
    bool due;
    {
      std::lock_guard<dxmt::mutex> lock(told_lock_);
      told_.looks++;
      if (hr == S_OK)
        told_.there++;
      else if (late)
        told_.stalled++;
      else
        told_.pending++;
      if (quiet)
        told_.longest = std::max(told_.longest, ended - began);
      due = ended - told_at_ > 200000000;
    }
    if (due)
      Told(4, 0);
    return hr;
  }

  void
  STDMETHODCALLTYPE
  Flush1(D3D11_CONTEXT_TYPE Type, HANDLE hEvent) override {
    OtlCall call(otl_call ? otl_call : OTL_WHY_FLUSH);
    Base::Flush1(Type, hEvent);
  }

  void
  PrepareFlush() override {
    OtlCall call(OTL_WHY_PRESENT);
    Base::PrepareFlush();
  }

  void
  STDMETHODCALLTYPE
  ExecuteCommandList(ID3D11CommandList *pCommandList, BOOL RestoreContextState) override {
    std::lock_guard<d3d11_device_mutex> lock(this->mutex);
    OtlCall call(OTL_WHY_COMMAND_LIST);
    const uint32_t former = otl_query_end;
    otl_query_end |= 4;
    if (otl_active()) // (the lists' other queries are ended by End)
      for (const auto &query : static_cast<MTLD3D11CommandList *>(pCommandList)->event_markers) {
        D3D11_QUERY_DESC desc;
        query->GetDesc(&desc);
        std::lock_guard<dxmt::mutex> told(told_lock_);
        told_.ended[desc.Query != D3D11_QUERY_EVENT]++;
      }
    Base::ExecuteCommandList(pCommandList, RestoreContextState);
    otl_query_end = former;
    if (otl_active() && this->promote_flush) {
      this->otl_asked_ = OTL_WHY_COMMAND_LIST;
      this->otl_asked_at_ = otl_now();
    }
  }

  void
  WaitUntilGPUIdle() override {
    OtlCall call(OTL_WHY_IDLE);
    Base::WaitUntilGPUIdle();
  }

  void
  Commit() override {
    const Committing here(this);
    Base::Commit();
    here.Told();
  }

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D11Fence *pFence, UINT64 Value) override {
    OtlCall call(OTL_WHY_SIGNAL);
    std::lock_guard<d3d11_device_mutex> lock(this->mutex);
    const uint64_t group = this->cmd_queue.otl_group_;
    const HRESULT hr = Base::Signal(pFence, Value);
    if (otl_active())
      Told(0, group);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D11Fence *pFence, UINT64 Value) override {
    OtlCall call(OTL_WHY_WAIT);
    return Base::Wait(pFence, Value);
  }

  HRESULT
  AcquireSync(ID3D11Resource *pResource, UINT64 Key, DWORD dwMilliseconds) override {
    OtlCall call(OTL_WHY_KEYED_MUTEX);
    return Base::AcquireSync(pResource, Key, dwMilliseconds);
  }

  HRESULT
  ReleaseSync(ID3D11Resource *pResource, UINT64 Key) override {
    OtlCall call(OTL_WHY_KEYED_MUTEX);
    return Base::ReleaseSync(pResource, Key);
  }

protected:
  void
  otlPassEnded(uint32_t why) override {
    if (otl_passes && this->cmd_queue.otl_group_ % otl_passes == 0)
      otl_put(otl_now(), OTL_D_PASS_ENDED, why, this->cmd_queue.otl_id_, this->cmd_queue.CurrentSeqId(), this->cmd_queue.otl_group_,
              this->otl_colour_, this->otl_colour_texture_, this->otl_depth_texture_);
  }

  template <typename Outcome>
  void
  RescueOutcome(Outcome outcome, uint64_t chunk, uint64_t event_id, uint64_t before, uint64_t after, uint64_t began, bool owed) {
    if (!otl_active())
      return;
    const uint64_t now = otl_now();
    if (outcome == Outcome::Done || outcome == Outcome::Uploads)
      otl_put(now, OTL_D_STALL, 2 | (event_id > after) << 2 | 8 | owed << 4, this->cmd_queue.otl_id_, chunk, event_id, before, after,
              this->cmd_queue.otl_group_);
    std::lock_guard<dxmt::mutex> lock(told_lock_);
    switch (outcome) {
    case Outcome::Uploads:
      told_.uploads++;
      [[fallthrough]];
    case Outcome::Done:
      told_.rescues++;
      told_.owed += owed;
      break;
    case Outcome::Covered:
      told_.covered += !owed;
      break;
    case Outcome::Lock:
      told_.lock += !owed;
      break;
    case Outcome::Queue:
      told_.full += !owed;
      break;
    }
    if (!owed)
      told_.longest_rescue = std::max(told_.longest_rescue, now - began);
  }

  struct Looking {
    QueryTrace *trace;
    uint32_t commits;
  };
  static inline thread_local Looking *looking = nullptr; // the look with DONOTFLUSH this thread is in

  /* A commit, under the device's lock where there is one: OTL_D_COMMIT, and of a look without the switch OTL_D_STALL. */
  struct Committing {
    explicit Committing(TimedImmediateContext *context) : of(context), otl(otl_active()) {
      if (!otl)
        return;
      at = otl_now();
      chunk = of->cmd_queue.CurrentSeqId();
      if (!Once && looking) {
        std::lock_guard<dxmt::mutex> lock(looking->trace->lock);
        event_id = looking->trace->event;
      }
    }
    void
    Told() const {
      const uint64_t before = of->submitted_.load(std::memory_order_relaxed), after = of->cmd_queue.GetCurrentEventSeqId();
      of->submitted_.store(after, std::memory_order_release);
      if (!otl)
        return;
      const uint64_t now = otl_now(), group = of->cmd_queue.otl_group_;
      otl_put(at, OTL_D_COMMIT, of->otl_why_ | of->otl_asked_ << 8 | uint32_t(of->otl_open_) << 16, of->cmd_queue.otl_id_, chunk, group,
              of->otl_asked_at_, now - at, of->otl_colour_);
      of->otl_asked_ = 0;
      of->otl_asked_at_ = 0;
      if (!Once && looking) {
        looking->commits++;
        otl_put(at, OTL_D_STALL, (event_id <= before) | 2 | (event_id > after) << 2, of->cmd_queue.otl_id_, chunk, event_id, before, after, group);
        std::lock_guard<dxmt::mutex> lock(of->told_lock_);
        of->told_.rescues++;
      }
    }
    TimedImmediateContext *const of;
    const bool otl;
    uint64_t at = 0, chunk = 0, event_id = 0;
  };

private:
  struct Counts {
    uint32_t ended[4] = {}, clean = 0, looks = 0, pending = 0, stalled = 0, there = 0, covered = 0, rescues = 0, owed = 0;
    uint32_t lock = 0, full = 0, uploads = 0;
    uint64_t longest = 0, longest_rescue = 0;
  };

  /* What was counted since the last time, all of one moment. */
  void
  Told(uint32_t how, uint64_t group) {
    const uint64_t now = otl_now();
    Counts c;
    {
      std::lock_guard<dxmt::mutex> lock(told_lock_);
      c = told_;
      told_ = {};
      told_at_ = now;
    }
    auto few = [](uint64_t n, unsigned shift) { return std::min<uint64_t>(n, 0xffff) << shift; };
    auto ns = [](uint64_t n) { return std::min<uint64_t>(n, 0xffffffff); };
    const uint32_t a = Once | this->mutex.get_protected() << 1 | how;
    const uint64_t queue = this->cmd_queue.otl_id_;
    otl_put(now, OTL_D_QUERIES, a, queue, group, few(c.ended[0], 0) | few(c.ended[1], 16) | few(c.ended[2], 32) | few(c.ended[3], 48),
            c.clean | uint64_t(c.lock + c.full) << 32, c.pending | uint64_t(c.stalled) << 32, c.covered | uint64_t(c.there) << 32);
    otl_put(now, OTL_D_LOOKS, a, queue, group, c.looks | uint64_t(c.pending + c.stalled) << 32, c.there | uint64_t(c.rescues) << 32,
            few(c.lock, 0) | few(c.full, 16) | few(c.uploads, 32) | few(c.owed, 48), ns(c.longest) | ns(c.longest_rescue) << 32);
  }

  dxmt::mutex told_lock_; // (a leaf)
  Counts told_;
  uint64_t told_at_ = otl_now();
  std::atomic<uint64_t> submitted_ = 0;
};

/* (RescueTold has the outcomes' type, which the context without the switch has not) */
class TimedImmediateContextOnce final : public TimedImmediateContext<MTLD3D11ImmediateContextOnce, true> {
public:
  using TimedImmediateContext::TimedImmediateContext;
  ~TimedImmediateContextOnce() { StopRescues(); } // (the rescue thread tells this class)

protected:
  void
  CommitRescue(uint64_t resource_initializer_event_id) override {
    const Committing here(this);
    MTLD3D11ImmediateContextOnce::CommitRescue(resource_initializer_event_id);
    here.Told();
  }

  void
  RescueTold(Rescue outcome, uint64_t chunk, uint64_t event_id, uint64_t before, uint64_t after, uint64_t began, bool owed) override {
    RescueOutcome(outcome, chunk, event_id, before, after, began, owed);
  }
};

std::unique_ptr<MTLD3D11DeviceContextBase>
InitializeImmediateContext(MTLD3D11Device *pDevice, CommandQueue &cmd_queue, QueryArm arm) {
  if (arm.once && arm.timed)
    return std::make_unique<TimedImmediateContextOnce>(pDevice, cmd_queue);
  if (arm.once)
    return std::make_unique<MTLD3D11ImmediateContextOnce>(pDevice, cmd_queue);
  if (arm.timed)
    return std::make_unique<TimedImmediateContext<MTLD3D11ImmediateContext, false>>(pDevice, cmd_queue);
  return std::make_unique<MTLD3D11ImmediateContext>(pDevice, cmd_queue);
}

}; // namespace dxmt
