// -*-C++-*-

#pragma once

#include <concepts>

#include <x23/utility.h>

namespace x23::detail {

// Derive most +/- operations from operator+= in the derived type.
struct PlusMinusMixin {
  using difference_type = ptrdiff_t;

  constexpr auto operator++(this auto &&self, int) noexcept
  {
    auto ret = self;
    self += 1;
    return ret;
  }
  constexpr auto operator--(this auto &&self, int) noexcept
  {
    auto ret = self;
    self += -1;
    return ret;
  }

  constexpr decltype(auto) operator++(this auto &&self) noexcept
  {
    return self += 1;
  }
  constexpr decltype(auto) operator--(this auto &&self) noexcept
  {
    return self += -1;
  }

  decltype(auto) operator-=(this auto &&self, difference_type d) noexcept
  {
    return self += -d;
  }

  constexpr auto operator+(this auto self, difference_type d) noexcept
  {
    self += d;
    return self;
  }
  constexpr auto operator-(this auto self, difference_type d) noexcept
  {
    self += -d;
    return self;
  }

  friend constexpr auto
  operator+(difference_type d,
            std::derived_from<PlusMinusMixin> auto self) noexcept
  {
    self += d;
    return self;
  }
  friend constexpr auto
  operator-(difference_type d,
            std::derived_from<PlusMinusMixin> auto self) noexcept
  {
    self += -d;
    return self;
  }
};

// Define operator[] and operator* in terms of operator-> and
// operator+=.
struct AccessMixin {
  constexpr decltype(auto) operator*(this const auto &self) noexcept
  {
    return *self.operator->();
  }

  constexpr decltype(auto) operator[](this auto self, ptrdiff_t d) noexcept
  {
    self += d;
    return *self;
  }
};

// Define random access iterator methods if you define:
//    - operator->
//    - operator+=
//    - friend difference_type operator-(iter, iter)
//    - operator==
//    - operator<=>
//
// Note: make operator- a friend rather than a method to avoid
// shadowing PlusMinuxMixin::operator-.
struct RandomAccessIterMixin : PlusMinusMixin, AccessMixin {};

} // namespace x23::detail
