// cpputil.h - low-level utilities for c++

#pragma once

#include <concepts>
#include <type_traits>
#include <typeinfo>

namespace hogd {

template<typename DerivedT, typename T>
requires std::derived_from<DerivedT, T> && std::is_polymorphic_v<T>
constexpr bool
is_most_derived(const T &x)
{
  return (typeid(x) == typeid(DerivedT));
}

// most_derived_cast casts a type to its most derived type or returns nullptr
// if DerivedT is not its most derived type.
//
// It is similar but faster than dynamic_cast. However, it can only cast to the
// most derived type (i.e., not intermediate types of an object with multiple
// inheritance). The base class must also be polymorphic, which is checked at
// compile time.
template<typename DerivedT, typename T>
DerivedT *
most_derived_cast(T *x)
{
  if (is_most_derived<DerivedT>(*x))
    return static_cast<DerivedT *>(x);
  return nullptr;
}

} // namespace hogd
