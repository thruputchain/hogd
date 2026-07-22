// lthash16.h - LtHash16 homomorphic multiset hash
//
// LtHash16 maintains a 2048-byte state (1024 uint16_t components). Elements
// can be incrementally added or removed:
//   H(A union B) = H(A) + H(B)
//   H(A setminus B) = H(A) - H(B)
//
// Each element is hashed with BLAKE3 in XOF mode to produce 2048 bytes,
// interpreted as 1024 little-endian uint16_t values, then added to or
// subtracted from the state component-wise modulo 2^16.

#pragma once

extern "C" {
#include "blake3.h"
}

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

static_assert(std::endian::native == std::endian::little,
              "LtHash16 assumes little-endian byte order");

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

  // Piecewise overloads: hash [key + type_byte + value] without requiring a
  // contiguous buffer.
  void add(x23::BufView key, uint8_t type, x23::BufView value)
  {
    apply_pieces(key, type, value, false);
  }
  void remove(x23::BufView key, uint8_t type, x23::BufView value)
  {
    apply_pieces(key, type, value, true);
  }

  void reset() { state_.fill(0); }

  const State &state() const { return state_; }

  LtHash16 &operator+=(const LtHash16 &other)
  {
    for (size_t i = 0; i < kNumComponents; ++i)
      state_[i] += other.state_[i];
    return *this;
  }

  LtHash16 &operator-=(const LtHash16 &other)
  {
    for (size_t i = 0; i < kNumComponents; ++i)
      state_[i] -= other.state_[i];
    return *this;
  }

  friend LtHash16 operator+(const LtHash16 &a, const LtHash16 &b)
  {
    LtHash16 r(a);
    r += b;
    return r;
  }

  friend LtHash16 operator-(const LtHash16 &a, const LtHash16 &b)
  {
    LtHash16 r(a);
    r -= b;
    return r;
  }

  friend bool operator==(const LtHash16 &, const LtHash16 &) = default;

private:
  void apply(x23::BufView element, bool negate)
  {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, element.data(), element.size());
    finalize(hasher, negate);
  }

  void apply_pieces(x23::BufView key, uint8_t type, x23::BufView value,
                    bool negate)
  {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, key.data(), key.size());
    blake3_hasher_update(&hasher, &type, 1);
    blake3_hasher_update(&hasher, value.data(), value.size());
    finalize(hasher, negate);
  }

  void finalize(blake3_hasher &hasher, bool negate)
  {
    // BLAKE3 writes bytes into the uint16_t component storage through a
    // uint8_t *, and we read them back as uint16_t. Accessing an object's
    // representation this way is only well-defined through a character type;
    // uint8_t is an optional typedef that the standard does not require to be
    // unsigned char. Guarantee the alias-permitted choice at compile time so
    // this consensus-hash code never depends on an implementation decision.
    static_assert(std::is_same_v<uint8_t, unsigned char>,
                  "LtHash16 finalize aliases uint16_t storage through "
                  "uint8_t *; uint8_t must be unsigned char");
    alignas(64) uint16_t components[kNumComponents];
    blake3_hasher_finalize(&hasher, reinterpret_cast<uint8_t *>(components),
                           kStateBytes);
    // clang and gcc both seem to vectorize this well
    // https://godbolt.org/z/q7Yr1essM
    if (negate) {
      for (size_t i = 0; i < kNumComponents; ++i)
        state_[i] -= components[i];
    }
    else {
      for (size_t i = 0; i < kNumComponents; ++i)
        state_[i] += components[i];
    }
  }

  State state_;
};

} // namespace hogd
