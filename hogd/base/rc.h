#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <utility>

namespace hogd {

// Rc<T> is a std::shared_ptr<T> whose reference count is not atomic. Copying an
// Rc shares the T. There are no weak references, custom deleters or aliasing
// constructors. Construct one with make_rc<T>(args...). Use Rc<const T> for a
// value that must not change after construction.
//
// std::shared_ptr's count is always atomic. An Rc's is not, so every Rc that
// shares a value must be used by only one thread at a time. State owned by a
// single transaction's execution meets that: it may resume on another thread
// after a suspension, but it never runs on two at once. Never let an Rc reach
// state that another thread can touch concurrently; use std::shared_ptr there.
template<typename T> class Rc {
public:
  // An empty Rc, which holds no value.
  Rc() = default;

  Rc(Rc const &other) noexcept : box_(other.box_)
  {
    if (box_) {
      ++box_->count_;
    }
  }
  Rc(Rc &&other) noexcept : box_(std::exchange(other.box_, nullptr)) {}

  Rc &operator=(Rc const &other) noexcept
  {
    if (this != &other) {
      Rc(other).swap(*this);
    }
    return *this;
  }
  Rc &operator=(Rc &&other) noexcept
  {
    Rc(std::move(other)).swap(*this);
    return *this;
  }

  ~Rc()
  {
    if (box_ && --box_->count_ == 0) {
      delete box_;
    }
  }

  T &operator*() const
  {
    assert(box_);
    return box_->value_;
  }
  T *operator->() const { return get(); }
  T *get() const { return box_ ? &box_->value_ : nullptr; }

  explicit operator bool() const { return box_ != nullptr; }

  // The number of Rcs sharing this value (0 if this Rc is empty).
  size_t use_count() const { return box_ ? box_->count_ : 0; }

  void swap(Rc &other) noexcept { std::swap(box_, other.box_); }

private:
  struct Box {
    template<typename... Args>
    explicit Box(Args &&...args) : value_(std::forward<Args>(args)...)
    {}

    size_t count_ = 1;
    T value_;
  };

  explicit Rc(Box *box) : box_(box) {}

  template<typename U, typename... Args>
  requires std::constructible_from<U, Args...>
  friend Rc<U> make_rc(Args &&...args);

  Box *box_ = nullptr;
};

// Allocates a T constructed from args, held by a new Rc.
template<typename T, typename... Args>
requires std::constructible_from<T, Args...>
Rc<T>
make_rc(Args &&...args)
{
  return Rc<T>(new typename Rc<T>::Box(std::forward<Args>(args)...));
}

} // namespace hogd
