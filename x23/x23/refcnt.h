// -*-C++-*-

#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
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

// Refcounted pointer for classes with incref()/decref() methods.  If
// you just want to delete at 0 reference count, use std::shared_ptr.
// If you need something else, then define `incref()` and `decref()`
// methods on your class, or define some other Ops adapter object for
// other methods.
template<typename T, typename Ops = detail::RefOps<T>> class RefPtr {
public:
  using element_type = T;
  using ops = Ops;

  // Classes that don't have:
  //
  //   static constexpr bool kAllowRefPtrConversion = true;
  //
  // need to provide a factory function that creates RefPtrs.
  template<std::same_as<T> U = T> requires (U::kAllowRefPtrConversion)
  RefPtr(T &t) : p_(&t)
  {
    ops::incref(p_);
  }

  RefPtr(std::nullptr_t) noexcept : p_(nullptr) {}
  RefPtr(const RefPtr &other) : RefPtr(*other) {}
  RefPtr(RefPtr &&other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
  ~RefPtr() { reset(); }
  RefPtr &operator=(std::nullptr_t)
  {
    reset();
    return *this;
  }
  decltype(auto) operator=(this auto &&self, const RefPtr &other) // NOLINT
  {
    if (self.p_ != other.p_) {
      if (self.p_)
        ops::decref(self.p_);
      if ((self.p_ = other.p_))
        ops::incref(self.p_);
    }
    return (self);
  }
  decltype(auto) operator=(this auto &&self, RefPtr &&other) noexcept // NOLINT
  {
    if (&self != &other) {
      self.reset();
      self.p_ = std::exchange(other.p_, nullptr);
    }
    return (self);
  }
  auto get() const noexcept { return ops::get(p_); }
  decltype(auto) operator*() const noexcept { return *get(); }
  auto operator->() const noexcept { return get(); }
  void reset()
  {
    if (p_)
      ops::decref(std::exchange(p_, nullptr));
  }

protected:
  template<std::same_as<T> U = T>
  requires (!requires { requires bool(U::kAllowRefPtrConversion); })
  RefPtr(T &t) : p_(&t)
  {
    ops::incref(p_);
  }

private:
  T *p_;

  friend T;
};
template<typename T> RefPtr(T &) -> RefPtr<T>;

// Base for classes that want to be managed by a RefPtr and deleted
// when the reference count goes to zero.
class RefCount {
protected:
  auto refcount() const noexcept { return count_.load(); }
  void finalize(this auto &self) noexcept { delete &self; }

private:
  std::atomic<unsigned> count_{0};

  void incref() noexcept { count_.fetch_add(1, std::memory_order_relaxed); }
  void decref(this auto &self) noexcept
  {
    auto &c = self.RefCount::count_;
    if (c.fetch_sub(1, std::memory_order_release) == 1) {
      // Synchronize with decrefs in other threads before freeing.
      c.load(std::memory_order_acquire);
      self.finalize();
    }
  }

  template<typename T> friend struct detail::RefOps;
};

// DestroyInhibit is a reference count that waits for all references
// to be released before destroy_wait() returns.
class DestroyInhibit {
public:
  static constexpr bool kAllowRefPtrConversion = true;

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
