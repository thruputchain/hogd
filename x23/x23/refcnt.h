// -*-C++-*-

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <stop_token>
#include <utility>

#include <x23/utility.h>

namespace x23 {

// Refcounted pointer for classes with incref()/decref() methods.  If
// you just want to delete at 0 reference count, use std::shared_ptr.
// If you need something else, then define `incref()` and `decref()`
// methods on your class.
template<typename T> class RefPtr {
public:
  using element_type = T;

  RefPtr(std::nullptr_t) noexcept : p_(nullptr) {}
  RefPtr(T &t) : p_(&t) { p_->incref(); }
  RefPtr(const RefPtr &other) : RefPtr(*other) {}
  RefPtr(RefPtr &&other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
  ~RefPtr() { clear(); }
  RefPtr &operator=(std::nullptr_t)
  {
    clear();
    return *this;
  }
  RefPtr &operator=(const RefPtr &other) // NOLINT
  {
    if (p_ != other.p_) {
      if (p_)
        p_->decref();
      p_ = other.p_;
      if (p_)
        p_->incref();
    }
    return *this;
  }
  RefPtr &operator=(RefPtr &&other) noexcept
  {
    clear();
    p_ = std::exchange(other.p_, nullptr);
    return *this;
  }
  T *get() const { return p_; }
  T &operator*() const { return *p_; }
  T *operator->() const { return p_; }
  void clear()
  {
    if (p_)
      std::exchange(p_, nullptr)->decref();
  }

private:
  T *p_;
};

// DestroyInhibit is a reference count that waits for all references
// to be released before destroy_wait() returns.
class DestroyInhibit {
public:
  DestroyInhibit() noexcept = default;
  DestroyInhibit(DestroyInhibit &&) = delete;
  ~DestroyInhibit() { assert(refcnt_.load(std::memory_order_relaxed) == 0); }

  void incref() { refcnt_.fetch_add(2, std::memory_order_relaxed); }
  void decref()
  {
    auto c = refcnt_.fetch_sub(2, std::memory_order_release) - 2;
    if (c == 1) {
      refcnt_.notify_all();
      refcnt_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
  void destroy_wait()
  {
    // Here we assume that once there are no more references, incref
    // will no longer be called
    auto c = refcnt_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c > 1) {
      do {
        refcnt_.wait(c, std::memory_order_relaxed);
        c = refcnt_.load(std::memory_order_relaxed);
      } while (c > 1);
      while (refcnt_.load(std::memory_order_relaxed) != 0)
        detail::pause_instruction();
    }
    else
      refcnt_.store(0, std::memory_order_relaxed);
    // Synchronize with all previous decrefs
    refcnt_.load(std::memory_order_acquire);
  }

private:
  // refcnt_ = 2 * num_refs + (notification_wanted ? 1 : 0)
  //
  // We jump through some hoops to avoid calling refcnt_.notify_all()
  // after refcnt_ has been destroyed.  (Even though notifying a
  // destroyed atomic would be fine with the underlying futex, it's
  // undefined behavior in C++.)  The strategy is for destroy_wait to
  // set the low bit, and for the last decref (if any are remaining)
  // to clear the low bit after notifying.  After setting the low bit,
  // if destroy_wait finds a non-zero reference count, it waits for
  // refcnt_ to be <= 1, then spins until it reaches 0 before
  // returning.
  std::atomic<unsigned> refcnt_{0};
};

// Calls a destructor function when the reference count hits zero and
// a stop has been requested on a std::stop_token.
struct DestroyTrigger {
  std::atomic<unsigned> refcnt_{0};
  std::function<void()> destructor_;
  std::stop_token st_;

  DestroyTrigger(std::stop_token st,
                 std::convertible_to<std::function<void()>> auto &&destructor)
    : destructor_(std::forward<decltype(destructor)>(destructor)),
      st_(std::move(st))
  {}

  void incref() noexcept { refcnt_.fetch_add(1, std::memory_order_relaxed); }
  void decref() noexcept
  {
    // Note: you must hold a reference when setting request_stop()
    if (refcnt_.fetch_sub(1, std::memory_order_release) == 1 &&
        st_.stop_requested()) {
      // Must ensure all other decrefs synchronize with the destructor.
      refcnt_.load(std::memory_order_acquire);
      destructor_();
    }
  }
};

} // namespace x23
