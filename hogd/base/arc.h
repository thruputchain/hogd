#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "x23/serialize.h"
#include "x23/x23.h"

namespace hogd {

// Arc<T> is a std::shared_ptr<T> that is never observably null. Copying an Arc
// shares the T, as copying a shared_ptr does: a change made through one copy
// is seen through every other. There are no weak references, custom deleters
// or aliasing constructors. Construct one with make_arc<T>(args...), from a T,
// or leave it empty.
//
// An empty Arc holds a value-initialized T: reading one gives T{}, a static
// that every empty Arc shares, and writing through one allocates its own T
// first. x23::Box works the same way, for the same reason: x23 decodes into a
// container of Arcs by value-initializing every element and then writing it,
// and a container resized from one empty Arc must not leave its elements
// sharing a T. share() is the one exception: on an empty Arc it points at the
// static T{} without owning it.
//
// The count is atomic, so copies of an Arc may be used from different threads.
// The T is not synchronized, so a T that is shared across threads must only be
// read there. Each Arc itself belongs to one thread, as a shared_ptr does.
//
// Decoding into an Arc writes its T in place, shared or not. Decode into a
// fresh Arc unless every holder should see the new value.
//
// Unlike Rc, Arc has x23's and std::tuple's protocols only for struct T:
// decoding a union needs union_ops, which only Box forwards.
template<typename T> class Arc {
public:
  using element_type = T;
  using value_type = T;

  // An empty Arc, which reads as T{} and allocates when written.
  Arc() noexcept = default;
  Arc(const Arc &) noexcept = default;
  Arc(Arc &&) noexcept = default;
  Arc &operator=(const Arc &) noexcept = default;
  Arc &operator=(Arc &&) noexcept = default;
  ~Arc() = default;

  // Holds a value. Implicit, like x23::Box, so a container of Arcs can be
  // written out as a list of values.
  Arc(T value) : ptr_(std::make_shared<T>(std::move(value))) {}

  template<typename... Args> requires std::constructible_from<T, Args...>
  explicit Arc(std::in_place_t /*unused*/, Args &&...args)
    : ptr_(std::make_shared<T>(std::forward<Args>(args)...))
  {}

  // Whether this Arc has allocated its value yet.
  bool has_value() const noexcept { return ptr_ != nullptr; }
  // The number of Arcs sharing this value (0 if this Arc is empty).
  size_t use_count() const noexcept
  {
    return static_cast<size_t>(ptr_.use_count());
  }

  // Reading through a const Arc never allocates; writing allocates first.
  T *get()
  {
    ensure();
    return ptr_.get();
  }
  const T *get() const noexcept
  {
    if (!has_value()) [[unlikely]]
      return &empty_value();
    return ptr_.get();
  }
  decltype(auto) operator*(this auto &&self) { return *self.get(); }
  auto operator->(this auto &&self) { return self.get(); }

  // The shared_ptr itself, allocating the value first if empty.
  std::shared_ptr<T> share()
  {
    ensure();
    return ptr_;
  }
  // The shared value, or for an empty Arc a pointer to the static T{} that
  // owns nothing.
  std::shared_ptr<const T> share() const noexcept
  {
    if (!has_value()) [[unlikely]]
      return std::shared_ptr<const T>(std::shared_ptr<const T>(),
                                      &empty_value());
    return ptr_;
  }

  void ensure()
  {
    if (!has_value())
      ptr_ = std::make_shared<T>();
  }

  static const T &empty_value() noexcept
  {
    static const T value{};
    return value;
  }

private:
  explicit Arc(std::shared_ptr<T> ptr) noexcept : ptr_(std::move(ptr)) {}

  template<typename U, typename... Args>
  requires std::constructible_from<U, Args...>
  friend Arc<U> make_arc(Args &&...args);

  std::shared_ptr<T> ptr_;
};
template<typename T> Arc(T) -> Arc<T>;

// Allocates a T constructed from args, held by a new Arc.
template<typename T, typename... Args>
requires std::constructible_from<T, Args...>
Arc<T>
make_arc(Args &&...args)
{
  return Arc<T>(std::make_shared<T>(std::forward<Args>(args)...));
}

// Arcs compare by value, as x23::Box does, with sharing as a fast path.
template<typename T>
inline bool
operator==(const Arc<T> &a, const Arc<T> &b)
{
  return a.get() == b.get() || *a == *b;
}

template<typename T>
inline std::compare_three_way_result_t<T>
operator<=>(const Arc<T> &a, const Arc<T> &b)
{
  if (a.get() == b.get())
    return std::compare_three_way_result_t<T>::equivalent;
  return *a <=> *b;
}

} // namespace hogd

// x23 treats an Arc<T> as the T it holds, as it does an x23::Box<T> and an
// Rc<T>: an Arc field has T's wire format, and decoding into an empty Arc
// allocates its T first. So an XDR struct can hold its nested structs by Arc,
// a vector of Arcs decodes straight from a vector of T, and either stands in
// for the T wherever an RPC argument or return expects one.

// NOLINTNEXTLINE(readability-identifier-naming)
template<x23::is_xdr_struct T>
struct x23::xdr_meta<hogd::Arc<T>> : x23::xdr_meta<T> {};

template<typename X, x23::serializes_like<X> T>
struct x23::serialize_equiv<X, hogd::Arc<T>> : std::true_type {};

template<typename T> requires requires { typename std::tuple_size<T>::type; }
struct std::tuple_size<hogd::Arc<T>> : std::tuple_size<T> {};

template<size_t I, typename T>
requires requires { typename std::tuple_element_t<I, T>; }
struct std::tuple_element<I, hogd::Arc<T>> : std::tuple_element<I, T> {};

namespace hogd {

template<size_t I, typename T> requires requires(T &t) { get<I>(t); }
decltype(auto)
get(Arc<T> &a)
{
  a.ensure();
  return get<I>(*a);
}

template<size_t I, typename T> requires requires(const T &t) { get<I>(t); }
decltype(auto)
get(const Arc<T> &a)
{
  return get<I>(*a);
}

} // namespace hogd
