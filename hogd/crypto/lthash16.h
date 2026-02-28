// lthash16.h - LtHash16 homomorphic multiset hash
//
// LtHash16 maintains a 2048-byte state (1024 uint16_t components). Elements
// can be incrementally added or removed:
//   H(A ∪ B) = H(A) + H(B)
//   H(A \ B) = H(A) - H(B)
//
// Each element is hashed with BLAKE3 in XOF mode to produce 2048 bytes,
// interpreted as 1024 little-endian uint16_t values, then added to or
// subtracted from the state component-wise modulo 2^16.

#pragma once

extern "C" {
#include "blake3.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "x23/vec.h"

namespace hogd {

class LtHash16 {
public:
  static constexpr size_t kNumComponents = 1024;
  static constexpr size_t kStateBytes = kNumComponents * sizeof(uint16_t);

  using State = std::array<uint16_t, kNumComponents>;

  LtHash16() { state_.fill(0); }
  explicit LtHash16(const State &state) : state_(state) {}

  void add(x23::BufView element) { apply(element, false); }
  void remove(x23::BufView element) { apply(element, true); }
  void reset() { state_.fill(0); }

  const State &state() const { return state_; }

  LtHash16 &
  operator+=(const LtHash16 &other)
  {
    for (size_t i = 0; i < kNumComponents; ++i)
      state_[i] += other.state_[i];
    return *this;
  }

  LtHash16 &
  operator-=(const LtHash16 &other)
  {
    for (size_t i = 0; i < kNumComponents; ++i)
      state_[i] -= other.state_[i];
    return *this;
  }

  friend LtHash16
  operator+(const LtHash16 &a, const LtHash16 &b)
  {
    LtHash16 r(a);
    r += b;
    return r;
  }

  friend LtHash16
  operator-(const LtHash16 &a, const LtHash16 &b)
  {
    LtHash16 r(a);
    r -= b;
    return r;
  }

  friend bool operator==(const LtHash16 &, const LtHash16 &) = default;

private:
  void
  apply(x23::BufView element, bool negate)
  {
    alignas(16) uint8_t buf[kStateBytes];

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, element.data(), element.size());
    blake3_hasher_finalize(&hasher, buf, sizeof(buf));

    const auto *components = reinterpret_cast<const uint16_t *>(buf);
    if (negate) {
      for (size_t i = 0; i < kNumComponents; ++i)
        state_[i] -= components[i];
    } else {
      for (size_t i = 0; i < kNumComponents; ++i)
        state_[i] += components[i];
    }
  }

  State state_;
};

} // namespace hogd
