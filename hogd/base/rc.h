#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <utility>

namespace hogd {

// Rc<T> is a reference-counted handle to a heap-allocated T, in the style of
// Rust's Rc<T>. Copying an Rc shares the T: it costs one increment, not a copy
// of the T.
//
// The count is not atomic, which is why this exists instead of
// std::shared_ptr (whose count is always atomic). Every Rc that shares a value
// must be used by only one thread at a time. State owned by a single
// transaction's execution meets that: it may resume on another thread after a
// suspension, but it never runs on two at once. Never let an Rc reach state
// that another thread can touch concurrently; use std::shared_ptr there.
//
// Access through a shared Rc is const. make_mut() gives mutable access, first
// copying the T if any other Rc shares it (copy-on-write), so a change made
// through one Rc is never seen through another.
//
// A moved-from Rc is empty. It may only be assigned to or destroyed.
template<typename T> class Rc {
public:
  template<typename... Args> requires std::constructible_from<T, Args...>
  explicit Rc(std::in_place_t /*unused*/, Args &&...args)
    : box_(new Box(std::forward<Args>(args)...))
  {}

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

  T const &operator*() const
  {
    assert(box_);
    return box_->value_;
  }
  T const *operator->() const
  {
    assert(box_);
    return &box_->value_;
  }

  // Mutable access to a T that no other Rc shares. If another Rc shares it,
  // this Rc first switches to a copy of its own.
  T &make_mut() requires std::copy_constructible<T>
  {
    assert(box_);
    if (box_->count_ != 1) {
      Rc(std::in_place, box_->value_).swap(*this);
    }
    return box_->value_;
  }

  // The number of Rcs sharing this T (0 if this Rc is empty).
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

  Box *box_;
};

} // namespace hogd
