// thread_checked_mutex.h - Mutex wrapper that detects cross-thread unlock
//
// Use this to catch bugs where a lock is held across a co_await and
// unlocked on a different thread (which is undefined behavior).

#pragma once

#include <cassert>
#include <thread>

#include "hogd/base/log.h"

namespace hogd {

// Wrapper around a mutex type that asserts unlock() is called from the
// same thread as lock(). This catches the common coroutine bug of holding
// a lock across co_await, where the coroutine may resume on a different thread.
//
// Works with std::unique_lock, std::scoped_lock, std::lock_guard, etc.
//
// Example:
//   ThreadCheckedMutex<std::mutex> mtx_;
//   ...
//   std::scoped_lock g(mtx_);  // Will assert if unlocked on different thread
//
template<typename Mutex> class ThreadCheckedMutex {
public:
  ThreadCheckedMutex() = default;
  ~ThreadCheckedMutex() = default;

  ThreadCheckedMutex(const ThreadCheckedMutex &) = delete;
  ThreadCheckedMutex &operator=(const ThreadCheckedMutex &) = delete;
  ThreadCheckedMutex(ThreadCheckedMutex &&) = delete;
  ThreadCheckedMutex &operator=(ThreadCheckedMutex &&) = delete;

  void lock()
  {
    mutex_.lock();
    lock_owner_ = std::this_thread::get_id();
  }

  void unlock()
  {
    check_owner();
    mutex_.unlock();
  }

  bool try_lock()
  {
    if (mutex_.try_lock()) {
      lock_owner_ = std::this_thread::get_id();
      return true;
    }
    return false;
  }

private:
  void check_owner()
  {
    if (std::this_thread::get_id() != lock_owner_) [[unlikely]] {
      // std::thread::id may not be formattable in all compilers,
      // so we just print a generic message
      panic("ThreadCheckedMutex::unlock() called from different thread than "
            "lock(). "
            "Did you hold a lock across co_await?");
    }
  }

  Mutex mutex_;
  std::thread::id lock_owner_;
};

} // namespace hogd
