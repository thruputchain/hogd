#pragma once

#include <concepts>
#include <limits>
#include <print>
#include <type_traits>

#include "hogd/base/compiler.h"

namespace hogd {

template<std::signed_integral T> class WideIntegral {
public:
  WideIntegral(T value)
    : highbits_(0), lowbits_(static_cast<unsigned_type>(value))
  {
    if (value < 0) {
      highbits_ = -1;
    }
  }

  WideIntegral &operator=(T value)
  {
    *this = WideIntegral(value);
    return *this;
  }

  WideIntegral &operator+=(T value)
  {
    if (integer_add_overflow<unsigned_type>(lowbits_, value, &lowbits_)) {

      if (value > 0) {
        highbits_++;
      }
      else {
        highbits_--;
      }
    }
    return *this;
  }

  WideIntegral &operator-=(T value)
  {
    if (integer_sub_overflow<unsigned_type>(lowbits_, value, &lowbits_)) {
      if (value > 0) {
        highbits_--;
      }
      else {
        highbits_++;
      }
    }
    return *this;
  }

  template<std::unsigned_integral OutType> bool is_in_range() const
  {
    static_assert(std::numeric_limits<unsigned_type>::max() >=
                  std::numeric_limits<OutType>::max());

    if (highbits_ != 0) {
      return false;
    }
    return lowbits_ <= std::numeric_limits<OutType>::max();
  }

  template<std::signed_integral OutType> bool is_in_range() const
  {
    static_assert(std::numeric_limits<unsigned_type>::max() >=
                  std::numeric_limits<OutType>::max());
    static_assert(
        static_cast<unsigned_type>(std::numeric_limits<OutType>::min()) + 1U +
            static_cast<unsigned_type>(std::numeric_limits<OutType>::max()) ==
        0);

    if (highbits_ >= 1) {
      return false;
    }
    if (highbits_ == 0) {
      return lowbits_ <=
             static_cast<unsigned_type>(std::numeric_limits<OutType>::max());
    }

    if (highbits_ <= -2) {
      return false;
    }

    // value is lowbits_ - 2^N
    // out_type::Max is 2^{n-1} -1
    // so if lowbits_ >= 2^{n-1}, then we'll be in range
    return lowbits_ >=
           std::numeric_limits<unsigned_type>::max() -
               static_cast<unsigned_type>(std::numeric_limits<OutType>::max());
  }

  template<std::integral OutType> OutType get_capped() const
  {
    if (is_in_range<OutType>()) {

      if constexpr (std::is_unsigned_v<OutType>) {
        return static_cast<OutType>(lowbits_);
      }
      else {
        if (lowbits_ <=
            static_cast<unsigned_type>(std::numeric_limits<OutType>::max())) {
          return static_cast<OutType>(lowbits_);
        }

        return static_cast<OutType>(lowbits_ -
                                    static_cast<unsigned_type>(
                                        std::numeric_limits<OutType>::max()) -
                                    1) +
               std::numeric_limits<OutType>::min();
      }
    }

    if (highbits_ >= 0) {
      return std::numeric_limits<OutType>::max();
    }
    return std::numeric_limits<OutType>::min();
  }

private:
  using unsigned_type = std::make_unsigned_t<T>;
  // endianness is meaningless for our use case
  T highbits_;
  unsigned_type lowbits_;
};

} // namespace hogd
