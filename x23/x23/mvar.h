// -*-C++-*-

#pragma once

#include <concepts>
#include <condition_variable>
#include <mutex>
#include <utility>

#include <x23/cleanup.h>

namespace x23 {

// Locked data structure, slightly harder to misuse than a separate
// mutex and variable.  Inspired by Haskell MVars.  The basic idea is
// that mvar.take() returns a Ptr that acts like a T* but releases the
// mutex on destruction.  Also operator-> returns a Ptr (that gets
// destroyed at the end of the full expression), making it safe to do
// something like ++mvar->field.  Finally, MVar<T>::with(F) calls an
// invocable of type F with a reference to the protected object,
// releasing the lock on completion.
template<typename T, typename Lock = std::mutex>
class alignas(hardware_destructive_interference_size) MVar {
public:
  using value_type = T;

  // Smart pointer to an object in an MVar that releases the lock when
  // destroyed.
  class Ptr {
  public:
    Ptr() noexcept = default;
    Ptr(MVar &mv)
    {
      mv.m_.lock();
      mv_ = &mv;
    }
    Ptr(MVar &mv, std::try_to_lock_t)
    {
      if (mv.m_.try_lock())
        mv_ = &mv;
    }
    Ptr(Ptr &&other) noexcept : mv_(std::exchange(other.mv_, nullptr)) {}
    ~Ptr() { reset(); }
    Ptr &operator=(std::nullptr_t) { reset(); }
    Ptr &operator=(Ptr &&other)
    {
      reset();
      mv_ = std::exchange(other.mv_, nullptr);
    }
    T *operator->() const { return &mv_->t_; }
    T &operator*() const { return mv_->t_; }
    explicit operator bool() const { return mv_; }
    void reset()
    {
      if (mv_) {
        auto &m = mv_->m_;
        mv_ = nullptr;
        m.unlock();
      }
    }

    void wait(std::condition_variable &c)
    {
      std::unique_lock ul(mv_->m_, std::adopt_lock);
      Defer _rel([&ul] { ul.release(); });
      c.wait(ul);
    }

  private:
    MVar *mv_{};
  };

  MVar() = default;
  template<typename... Args> requires std::constructible_from<T, Args...>
  MVar(Args &&...args) noexcept(noexcept(T(std::forward<Args>(args)...)))
    : t_(std::forward<Args>(args)...)
  {}

  // take() acquries the lock and returns a smart pointer Ptr to the
  // object that will release the lock when the pointer is destroyed
  // or when you call reset() on it.  The Ptr returned by take() will
  // always be non-null, so you can hold the lock for a block with:
  //
  //     if (auto ptr = mvar.take()) {
  //       // access object as *ptr or ptr->fields
  //     }
  Ptr take() { return *this; }

  // Like take(), but returns a NULL smart pointer if the lock is
  // unavailable.
  Ptr try_take() { return {*this, std::try_to_lock}; }

  // Given an MVar<T> mvar, you can directly access fields of the
  // underlying T as in mvar->field.  The lock will be held by a
  // temporary Ptr for the duration of the full expression.  Note,
  // however, that this can cause deadlock if you do it more than once
  // in the same full expression:
  //
  //    auto count = ++mvar->field;           // OK
  //    auto v = mvar->field1 + mvar->field2; // Deadlock
  //
  // The second would need to be rewritten as, for instance:
  //
  //    auto v = mvar.with([](auto &t) { return t.field1 + t.field2; });
  Ptr operator->() { return *this; }

  // Invoke a callable with a reference to the object protected by the
  // MVar, while holding the lock for the duration of the call.
  template<typename F>
  decltype(auto) with(F &&f)
      requires std::invocable<F, T &> &&
               (!std::invocable<F, T &, std::unique_lock<std::mutex>>)
  {
    std::lock_guard _g(m_);
    return std::invoke(std::forward<F>(f), t_);
  }

  // If the callable passed to with takes a
  // std::unique_lock<std::mutex> as a second argument, it will be
  // passed one holding the lock.  This can be useful in conjunction
  // with condition variables, but see Ptr::wait.
  template<typename F>
  decltype(auto) with(F &&f)
      requires std::invocable<F, T &, std::unique_lock<std::mutex>>
  {
    return std::invoke(std::forward<F>(f), t_, std::unique_lock(m_));
  }

private:
  Lock m_{};
  T t_{};
};

template<typename Lock> class MVar<void, Lock> {
public:
  using value_type = void;

  class Ptr {
  public:
    Ptr() noexcept {}
    Ptr(MVar &mv)
    {
      mv.m_.lock();
      mv_ = &mv;
    }
    Ptr(MVar &mv, std::try_to_lock_t)
    {
      if (mv.m_.try_lock())
        mv_ = &mv;
    }
    Ptr(Ptr &&other) noexcept : mv_(std::exchange(other.mv_, nullptr)) {}
    ~Ptr() { reset(); }
    Ptr &operator=(std::nullptr_t) { reset(); }
    Ptr &operator=(Ptr &&other)
    {
      reset();
      mv_ = std::exchange(other.mv_, nullptr);
    }
    explicit operator bool() const { return mv_; }
    void reset()
    {
      if (mv_) {
        auto &m = mv_->m_;
        mv_ = nullptr;
        m.unlock();
      }
    }

    void wait(std::condition_variable &c)
    {
      std::unique_lock ul(mv_->m_, std::adopt_lock);
      Defer _rel([&ul] { ul.release(); });
      c.wait(ul);
    }

  private:
    MVar *mv_{};
  };

  MVar() noexcept = default;

  Ptr take() { return *this; }
  Ptr try_take() { return {*this, std::try_to_lock}; }

  template<typename F> decltype(auto) with(F &&f) requires std::invocable<F>
  {
    std::lock_guard _g(m_);
    return std::invoke(std::forward<F>(f));
  }
  template<typename F>
  decltype(auto) with(F &&f)
      requires std::invocable<F, std::unique_lock<std::mutex>>
  {
    return std::invoke(std::forward<F>(f), std::unique_lock(m_));
  }

private:
  Lock m_;
};

} // namespace x23
