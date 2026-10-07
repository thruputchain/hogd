#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>

#include "x23/x23.h"

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
  T *get() const { return box_ ? std::addressof(box_->value_) : nullptr; }

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

// x23 treats an Rc<T> as the T it holds, as it does an x23::Box<T>: an Rc field
// has T's wire format, and decoding into an empty Rc allocates its T first. So
// an XDR struct can hold its nested structs by Rc, and x23 decodes straight
// into them.

// NOLINTNEXTLINE(readability-identifier-naming)
template<typename T> struct x23::xdr_meta<hogd::Rc<T>> : x23::xdr_meta<T> {};

template<typename T> requires requires { typename std::tuple_size<T>::type; }
struct std::tuple_size<hogd::Rc<T>> : std::tuple_size<T> {};

template<size_t I, typename T>
requires requires { typename std::tuple_element_t<I, T>; }
struct std::tuple_element<I, hogd::Rc<T>> : std::tuple_element<I, T> {};

namespace hogd {

template<size_t I, typename T> requires requires(T &t) { get<I>(t); }
decltype(auto)
get(Rc<T> &r)
{
  if (!r) {
    r = make_rc<T>();
  }
  return get<I>(*r);
}

template<size_t I, typename T> requires requires(T const &t) { get<I>(t); }
decltype(auto)
get(Rc<T> const &r)
{
  assert(r);
  return get<I>(std::as_const(*r));
}

} // namespace hogd
