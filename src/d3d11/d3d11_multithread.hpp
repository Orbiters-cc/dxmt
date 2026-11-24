#pragma once
#include "d3d11_4.h"
#include <atomic>

namespace dxmt {

class d3d11_device_mutex {
public:
  void lock();
  /* Never waits. (Not protected, as lock(): nothing is locked, and true.) */
  bool try_lock();
  void unlock() noexcept;
  /* This thread is in it. (Of a lock that is on for good alone: D3D11MultithreadOnce.) */
  bool owned();

  bool set_protected(bool Protected);
  bool get_protected();

private:
  bool protected_ = false;
  std::atomic_uint32_t owner_ = 0;
  uint32_t counter_ = 0;
};

class D3D11Multithread : public ID3D11Multithread {
public:
  D3D11Multithread(IUnknown *container, d3d11_device_mutex &mutex)
      : container_(container), mutex_(mutex) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() final { return container_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() final { return container_->Release(); }

  void STDMETHODCALLTYPE Enter() final { mutex_.lock(); }

  void STDMETHODCALLTYPE Leave() final { mutex_.unlock(); }

  WINBOOL STDMETHODCALLTYPE SetMultithreadProtected(WINBOOL Enable) final {
    return mutex_.set_protected(Enable);
  }

  WINBOOL STDMETHODCALLTYPE GetMultithreadProtected() final {
    return mutex_.get_protected();
  }

private:
  IUnknown *container_;
  d3d11_device_mutex &mutex_;
};

/**
A device's ID3D11Multithread with OBELISK_DXMT_QUERY_RESCUE=once (d3d11_query_rescue.hpp).

The lock itself is turned on before the device's context is made and never again off (MTLD3D11DeviceImpl): the rescue thread relies on it,
and an application may take the protection back at any moment, from any thread, with a rescue owed or under way. What it set is kept here,
beside the lock, for what it can see of it: what SetMultithreadProtected and GetMultithreadProtected say, and whether Enter holds anything.
Setting it waits for nobody and changes nothing of the lock, but that a thread that turns it off lets go of what it has itself entered
(no Leave would have, until now).

A Leave lets go of what this thread has entered, whatever was set since. One that has entered nothing lets go of nothing.
*/
class D3D11MultithreadOnce final : public ID3D11Multithread {
public:
  struct Asked {
    std::atomic<bool> on = false;
    uint32_t entered = 0; // (of the thread in the lock, which alone reads or writes it)
  };

  D3D11MultithreadOnce(IUnknown *container, d3d11_device_mutex &mutex, Asked &asked)
      : container_(container), mutex_(mutex), asked_(asked) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() final { return container_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() final { return container_->Release(); }

  void STDMETHODCALLTYPE Enter() final {
    if (!asked_.on.load(std::memory_order_acquire))
      return;
    mutex_.lock();
    asked_.entered++;
  }

  void STDMETHODCALLTYPE Leave() final {
    if (!mutex_.owned() || !asked_.entered)
      return;
    asked_.entered--;
    mutex_.unlock();
  }

  WINBOOL STDMETHODCALLTYPE SetMultithreadProtected(WINBOOL Enable) final {
    const bool ret = asked_.on.exchange(Enable != FALSE, std::memory_order_acq_rel);
    if (!Enable)
      while (mutex_.owned() && asked_.entered)
        Leave();
    return ret;
  }

  WINBOOL STDMETHODCALLTYPE GetMultithreadProtected() final {
    return asked_.on.load(std::memory_order_acquire);
  }

private:
  IUnknown *container_;
  d3d11_device_mutex &mutex_;
  Asked &asked_;
};

} // namespace dxmt
