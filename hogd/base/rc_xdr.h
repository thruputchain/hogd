#pragma once

#include <cassert>
#include <cstddef>
#include <tuple>
#include <utility>

#include "hogd/base/rc.h"
#include "x23/x23.h"

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
