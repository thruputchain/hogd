// -*- C++ -*-

#pragma once

#include <atomic>
#include <cassert>
#include <functional>
#include <utility>

namespace x23 {

class RawSignalCbHandle {
public:
  void cancel()
  {
    assert(!delflag_->load(std::memory_order_relaxed));
    *delflag_ = true;
  }

protected:
  RawSignalCbHandle(std::atomic_bool *d) noexcept : delflag_(d) {}
  std::atomic_bool *delflag_;

  friend class AutoSignalCbHandle;
  friend RawSignalCbHandle signal_cb(int, std::function<void()>);
};

class AutoSignalCbHandle : RawSignalCbHandle {
public:
  AutoSignalCbHandle() noexcept : RawSignalCbHandle(nullptr) {}
  AutoSignalCbHandle(const RawSignalCbHandle &raw) : RawSignalCbHandle(raw) {}
  AutoSignalCbHandle(AutoSignalCbHandle &&other) noexcept
    : RawSignalCbHandle(other.release())
  {}
  ~AutoSignalCbHandle()
  {
    if (delflag_)
      cancel();
  }
  RawSignalCbHandle release() noexcept
  {
    return std::exchange(delflag_, nullptr);
  }
};

// Register a callback to run on a particular signal.  If you want the
// callback permanently enabled, just ignore the return value.
// Otherwise, assign it to an AutoSignalCbHandle, and the callback
// will be deregistered when the AutoSignalCbHandle is destroyed.
//
// Note that a dedicated thread services all signal callbacks, so if a
// callback does not return, no other signals will be handled.
//
// You can register multiple callbacks for the same signal and they
// will all run.  However, signal_cb does not compose with other uses
// of the signal or sigaction system call--it assumes it is the only
// software installing a handler for any signals it is managing.
RawSignalCbHandle signal_cb(int signal, std::function<void()> cb);

} // namespace x23
