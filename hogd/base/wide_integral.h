#pragma once

#include <concepts>
#include <limits>
#include <type_traits>

#include "hogd/base/compiler.h"

namespace hogd {
// WideIntegral represents a wide integer type that can store values beyond the
// range of the underlying integral type T. It maintains the value across two
// fields - highbits_ and lowbits_ - allowing for arithmetic operations that
// would normally overflow.
//
// The class provides:
// - Construction from and assignment to the base type T
// - Addition and subtraction operators that handle overflow correctly
// - Range checking to determine if the value can fit in various integer types
// - Safe conversion to other integer types with capping at min/max values
//
// Template parameter T must be an integral type.
//
// Example:
//   WideIntegral<uint8_t> w(100);
//   w += 200;  // Value is now 300, beyond uint8_t range but stored correctly
//   bool fits = w.is_in_range<uint8_t>();  // false
//   uint8_t capped = w.get_capped<uint8_t>(); // 255
template<std::integral T> class WideIntegral {
public:
  // Constructs a WideIntegral from a value of the base type T.
  // For negative values, sets highbits_ to -1 to properly represent the sign.
  template<std::integral U>
  WideIntegral(U value)
    : highbits_(0), lowbits_(static_cast<unsigned_type>(value))
  {
    static_assert(sizeof(U) <= sizeof(T),
                  "input type is too large to fit in one arm of wide_integral");
    if constexpr (std::is_signed_v<U>) {
      if (value < 0) {
        highbits_ = -1;
      }
    }
  }
  WideIntegral() : highbits_(0), lowbits_(0) {}

  // Assignment operator that creates a new WideIntegral from the given value
  // and assigns it to this object.
  template<std::integral U> WideIntegral &operator=(U value)
  {
    static_assert(sizeof(U) <= sizeof(T),
                  "input type is too large to fit in one arm of wide_integral");
    *this = WideIntegral(value);
    return *this;
  }

  // Addition assignment operator that adds the given value to this
  // WideIntegral. Handles overflow by incrementing/decrementing highbits_ as
  // needed.
  WideIntegral &operator+=(std::signed_integral auto value)
  {
    static_assert(sizeof(decltype(value)) <= sizeof(T),
                  "input type is too large to fit in one arm of wide_integral");
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

  // Subtraction assignment operator that subtracts the given value from this
  // WideIntegral. Handles overflow by incrementing/decrementing highbits_ as
  // needed.
  WideIntegral &operator-=(std::signed_integral auto value)
  {
    static_assert(sizeof(decltype(value)) <= sizeof(T),
                  "input type is too large to fit in one arm of wide_integral");
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
  // Checks if the current value can fit within the range of the given unsigned
  // integral type. Returns true if the value can be represented in OutType
  // without overflow. Returns false if the value is negative or exceeds
  // OutType's maximum value.
  //
  // The static_assert ensures that the current unsigned_type can hold values at
  // least as large as OutType's maximum value.
  template<std::unsigned_integral OutType> bool is_in_range() const
  {
    static_assert(std::numeric_limits<unsigned_type>::max() >=
                  std::numeric_limits<OutType>::max());

    if (highbits_ != 0) {
      return false;
    }
    return lowbits_ <= std::numeric_limits<OutType>::max();
  }

  // Checks if the current value can fit within the range of the given signed
  // integral type. Returns true if the value can be represented in OutType
  // without overflow/underflow. Returns false if the value exceeds OutType's
  // maximum or minimum value.
  //
  // The static_asserts ensure:
  // 1. The current unsigned_type can hold values at least as large as OutType's
  // maximum
  // 2. The two's complement representation invariant holds for OutType
  template<std::signed_integral OutType> bool is_in_range() const
  {
    static_assert(std::numeric_limits<unsigned_type>::max() >=
                  std::numeric_limits<OutType>::max());
    static_assert(
        static_cast<unsigned_type>(
            static_cast<unsigned_type>(std::numeric_limits<OutType>::min()) +
            1U +
            static_cast<unsigned_type>(std::numeric_limits<OutType>::max())) ==
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

  // Safely converts the current value to the given integral type OutType.
  // If the value fits within OutType's range (checked via
  // is_in_range<OutType>()), returns the exact value. Otherwise returns:
  // - OutType's maximum value if the current value is positive
  // - OutType's minimum value if the current value is negative
  //
  // For unsigned OutTypes, directly casts lowbits_.
  // For signed OutTypes, handles two's complement conversion when needed.
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
  using signed_type = std::make_signed_t<T>;
  using unsigned_type = std::make_unsigned_t<T>;
  // private data; so endianness irrelevant
  signed_type highbits_;
  unsigned_type lowbits_;
};

} // namespace hogd
