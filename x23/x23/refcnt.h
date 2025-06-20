// -*-C++-*-

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <utility>

#include <x23/utility.h>

namespace x23 {

namespace detail {

template<typename T> struct RefOps {
  static void incref(T *p) noexcept(noexcept(p->incref())) { p->incref(); }
  static void decref(T *p) noexcept(noexcept(p->decref())) { p->decref(); }
  static T *get(T *p) noexcept { return p; }
};

} // namespace detail

class RefCount {
protected:
  // An object is created with a 0 reference count and only gets
  // deleted when the refernce count is first raised then lowered to
  // zero.  If you don't know whether any references have been kept,
  // you can delete the object with incdec().
  void incdec()
  {
    if (count_.load(std::memory_order_relaxed) == 0)
      delete this;
  }

  virtual ~RefCount() = default;

private:
  template<typename T> friend struct detail::RefOps;

  std::atomic_uintptr_t count_{0};

  void incref() { count_.fetch_add(1, std::memory_order_relaxed); }

  void decref()
  {
    if (count_.fetch_sub(1, std::memory_order_release) != 1)
      return;
    // Have to synchronize with releases in other threads before
    // we clobber the object memory.
    count_.load(std::memory_order_acquire);
    delete this;
  }
};

// Refcounted pointer for classes with incref()/decref() methods.  If
// you just want to delete at 0 reference count, use std::shared_ptr.
// If you need something else, then define `incref()` and `decref()`
// methods on your class, or define some other Ops adapter object for
// other methods.
template<typename T, typename Ops = detail::RefOps<T>> class RefPtr {
public:
  using element_type = T;
  using ops = Ops;

  RefPtr(std::nullptr_t) noexcept : p_(nullptr) {}
  RefPtr(T &t) : p_(&t) { ops::incref(p_); }
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
        ops::decref(p_);
      p_ = other.p_;
      if (p_)
        ops::incref(p_);
    }
    return *this;
  }
  RefPtr &operator=(RefPtr &&other) noexcept
  {
    if (this != &other) {
      clear();
      p_ = std::exchange(other.p_, nullptr);
    }
    return *this;
  }
  auto get() const noexcept { return ops::get(p_); }
  decltype(auto) operator*() const noexcept { return *get(); }
  auto operator->() const noexcept { return get(); }
  void clear()
  {
    if (p_)
      ops::decref(std::exchange(p_, nullptr));
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

} // namespace x23
