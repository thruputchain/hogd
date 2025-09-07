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

template<typename Ptr> struct RefOps {
  static auto incref(Ptr p) noexcept(noexcept(p->incref()))
  {
    return p->incref();
  }
  using tag_type = decltype(incref(declval<Ptr>()));

  static void decref(Ptr p, auto... tag) noexcept(noexcept(p->decref(tag...)))
      requires (sizeof...(tag) == !std::is_void_v<tag_type>)
  {
    p->decref(tag...);
  }

  static Ptr get(Ptr p) noexcept { return p; }
};

// In the event that we bucket reference counts by CPU, we want to be
// able to include a tag in a RefPtr recording the CPU or bucket in
// which the reference count was incremeneted.
template<typename Ptr, typename Ops> class RefTag {
public:
  using ops = Ops;
  using pointer_type = Ptr;
  using element_type = std::remove_reference_t<decltype(*declval<Ptr>())>;
  static constexpr bool kTagged = false;

protected:
  pointer_type p_{};

  void ref() { ops::incref(p_); }
  static void unref(pointer_type p) { ops::decref(p); }
  void movefrom(RefTag &other) noexcept
  {
    using std::exchange;
    p_ = exchange(other.p_, nullptr);
  }
};

template<typename Ptr, typename Ops>
requires requires(Ptr p) { Ops::decref(p, Ops::incref(p)); }
class RefTag<Ptr, Ops> {
public:
  using ops = Ops;
  using pointer_type = Ptr;
  using element_type = std::remove_reference_t<decltype(*declval<Ptr>())>;
  using tag_type = decltype(ops::incref(declval<Ptr>()));
  static constexpr bool kTagged = true;

protected:
  pointer_type p_{};
  tag_type tag_;

  void ref() { tag_ = ops::incref(p_); }
  void unref(pointer_type p) { ops::decref(p, tag_); }
  void movefrom(RefTag &other) noexcept
  {
    using std::exchange;
    tag_ = other.tag_;
    p_ = exchange(other.p_, nullptr);
  }
};

} // namespace detail

// Refcounted pointer for classes with incref()/decref() methods.  For
// types derived from RefCount, this will act like a std::shared_ptr,
// except you can use AtomicRefPtr instead of relying on
// std::atomic<std::shared_ptr<T>> (which in GCC just has a set of 16
// global mutexes).
//
// You can of course generalize `incref()` and `decref()` methods on
// your class, or define some other Ops adapter type for other
// methods.
template<typename T, typename Ops = detail::RefOps<T *>>
class RefPtr : public detail::RefTag<T *, Ops> {
  using super = detail::RefTag<T *, Ops>;
  using super::kTagged;
  using super::movefrom;
  using super::p_;
  using super::ref;
  using super::unref;

public:
  using ops = super::ops;
  using typename super::element_type;
  using typename super::pointer_type;

  // Classes that don't have:
  //
  //   static constexpr bool kAllowRefPtrConversion = true;
  //
  // need to provide a factory function that creates RefPtrs.
  template<std::same_as<T> U = T> requires (U::kAllowRefPtrConversion)
  RefPtr(T &t)
  {
    p_ = &t;
    ref();
  }

  RefPtr() noexcept = default;
  RefPtr(std::nullptr_t) : RefPtr() {}
  RefPtr(const RefPtr &other)
  {
    p_ = other.p_;
    ref();
  }
  RefPtr(RefPtr &&other) noexcept { movefrom(other); }

  ~RefPtr() { reset(); }

  decltype(auto) operator=(this is_mutable auto &&self,
                           std::nullptr_t) noexcept // NOLINT
  {
    self.reset();
    return (self);
  }
  decltype(auto) operator=(this is_mutable auto &&self,
                           const RefPtr &other) // NOLINT
  {
    if (self.p_ != other.p_) {
      if (self.p_)
        unref(self.p_);
      if ((self.p_ = other.p_))
        ref(self.p_);
    }
    return (self);
  }
  decltype(auto) operator=(this is_mutable auto &&self,
                           RefPtr &&other) noexcept // NOLINT
  {
    if (&self != &other) {
      self.reset();
      self.movefrom(other);
    }
    return (self);
  }
  auto get() const noexcept { return ops::get(this->p_); }
  decltype(auto) operator*() const noexcept { return *get(); }
  auto operator->() const noexcept { return get(); }
  void reset()
  {
    if (this->p_)
      this->unref(std::exchange(this->p_, nullptr));
  }
  explicit operator bool() const noexcept { return this->p_; }

  friend void swap(RefPtr &a, RefPtr &b) noexcept {
    using std::swap;
    if constexpr (kTagged)
      swap(a.tag_, b.tag_);
    swap(a.p_, b.p_);
  }

protected:
  template<std::same_as<T> U = T>
  requires (!requires { requires bool(U::kAllowRefPtrConversion); })
  RefPtr(T &t)
  {
    this->p_ = &t;
    this->ref();
  }

  friend T;
  template<typename U, typename O> friend class AtomicRefPtr;
};

namespace detail {

template<typename T, typename NonAtomicOps> struct AtomicRefOps {
  static auto load(std::atomic<T *> &p) noexcept { return p.load(); }
  static auto get(std::atomic<T *> &p) noexcept
  {
    return NonAtomicOps::get(load(p));
  }
  static auto incref(std::atomic<T *> &p)
  {
    return NonAtomicOps::incref(load(p));
  }
  static auto decref(std::atomic<T *> &p, auto... tag)
      requires (sizeof...(tag) ==
                !std::is_void_v<typename NonAtomicOps::tag_type>)
  {
    return NonAtomicOps::decref(load(p), tag...);
  }
};

} // namespace detail

// A way to generate RefPtr values without racing with a thread that
// might drop the last reference.  Uses the low bit of the pointer as
// a lock, so only for types T that have an alignof(T) >= 2.
template<typename T, typename Ops = detail::RefOps<T *>>
class AtomicRefPtr
  : public detail::RefTag<std::atomic<T *>, detail::AtomicRefOps<T, Ops>> {
  using super = detail::RefTag<std::atomic<T *>, detail::AtomicRefOps<T, Ops>>;
  using super::kTagged;
  using super::p_;

public:
  using NonAtomic = RefPtr<T, Ops>;

  AtomicRefPtr() noexcept = default;
  AtomicRefPtr(NonAtomic np) { store(std::move(np)); }
  AtomicRefPtr(const AtomicRefPtr &) noexcept = delete;
  ~AtomicRefPtr() { exchange(nullptr); }

  NonAtomic load() noexcept
  {
    auto p = take();
    auto ret = makeref(p);
    put(p);
    return ret;
  }

  void store(NonAtomic np) noexcept { exchange(std::move(np)); }

  NonAtomic exchange(NonAtomic np) noexcept
  {
    T *p = take();
    swaplocked(p, np);
    return np;
  }

  bool compare_exchange(NonAtomic &desired, T *expected) noexcept
  {
    T *p = take();
    if (p == expected) {
      auto copy(desired);
      swaplocked(p, copy);
      return true;
    }
    desired = makeref(p);
    put(p);
    return false;
  }

private:
  static_assert(alignof(T) % 2 == 0);
  template<uintptr_t V> requires (bool(V & 1))
  struct Sentinel {
    operator T *() const noexcept { return reinterpret_cast<T *>(V); }
  };
  static constexpr Sentinel<1> kEmpty{};
  static constexpr Sentinel<3> kEmptyWantNotify{};

  static NonAtomic makeref(T *p) noexcept
  {
    return p ? NonAtomic(*p) : nullptr;
  }

  T *take() noexcept
  {
    for (;;) {
      auto p = p_.load(std::memory_order_relaxed);
      if (p != kEmpty && p != kEmptyWantNotify) {
        if (p_.compare_exchange_weak(p, kEmpty, std::memory_order_acquire,
                                     std::memory_order_relaxed))
          return p;
        continue;
      }

      if (p == kEmpty && !p_.compare_exchange_weak(p, kEmptyWantNotify,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
        continue;

      p_.wait(kEmptyWantNotify);
    }
  }

  void put(T *p) noexcept
  {
    auto old = p_.exchange(p, std::memory_order_release);
    if (old == kEmptyWantNotify)
      p_.notify_all();
    else
      assert(old == kEmpty);
  }

  // Swap na into the atomic (unlocking the AtomicRefPtr>), and then
  // set na to lockedp.
  void swaplocked(T *lockedp, NonAtomic &na)
  {
    using std::swap;
    if constexpr (kTagged)
      swap(this->tag_, na.tag_);
    put(na.p_);
    na.p_ = lockedp;
  }
};

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
