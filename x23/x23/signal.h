// -*- C++ -*-

#pragma once

#include <atomic>
#include <cassert>
#include <functional>
#include <utility>

namespace x23 {

// A class you can either ignore or convert into an AutoSignalCbHandle
class RawSignalCbHandle {
protected:
  std::atomic_bool *delflag_;

  RawSignalCbHandle(std::atomic_bool *d) noexcept : delflag_(d) {}

  // Delete the signal callback.
  void cancel();

  friend class AutoSignalCbHandle;
  friend RawSignalCbHandle signal_cb(int, std::function<void()>);
};

// A reference to a signal callback that cancels the callback when the
// AutoSignalCbHandle is destroyed.
class AutoSignalCbHandle : RawSignalCbHandle {
public:
  AutoSignalCbHandle() noexcept : RawSignalCbHandle(nullptr) {}
  AutoSignalCbHandle(AutoSignalCbHandle &&other) noexcept
    : RawSignalCbHandle(std::exchange(other.delflag_, nullptr))
  {}
  AutoSignalCbHandle(RawSignalCbHandle &&other)
    : RawSignalCbHandle(std::exchange(other.delflag_, nullptr))
  {}
  AutoSignalCbHandle &operator=(AutoSignalCbHandle &&other) noexcept
  {
    delflag_ = std::exchange(other.delflag_, nullptr);
    return *this;
  }
  AutoSignalCbHandle &operator=(RawSignalCbHandle &&other) noexcept
  {
    delflag_ = std::exchange(other.delflag_, nullptr);
    return *this;
  }
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
