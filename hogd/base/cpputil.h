// cpputil.h - low-level utilities for c++

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <string_view>
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

// StringLiteral provides a string structural type that can be passed as a
// template parameter. It is parameterized on the character type so it can carry
// either an ordinary (`char`) or a UTF-8 (`char8_t`) literal. Callers that hash
// the bytes into protocol data must pass a `u8""` literal, so the stored code
// units are the standard's charset-independent UTF-8 encoding rather than the
// implementation-defined ordinary execution-charset encoding
// (`-fexec-charset`).
template<std::size_t N, typename CharT = char> struct StringLiteral {
  CharT buf[N];

  constexpr StringLiteral(const CharT (&str)[N]) { std::copy_n(str, N, buf); }
  constexpr std::basic_string_view<CharT> get() const
  {
    return std::basic_string_view<CharT>(buf, N - 1);
  }
};
template<size_t N> StringLiteral(const char (&)[N]) -> StringLiteral<N, char>;
template<size_t N>
StringLiteral(const char8_t (&)[N]) -> StringLiteral<N, char8_t>;

} // namespace hogd
