#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace hog {

/*! Typesafe way of storing the length of a key in bits.
Too many bugs were caused by accidentaly bits/bytes unit conversions.
*/
struct PrefixLenBits {

  uint16_t len;

  constexpr std::strong_ordering operator<=>(const PrefixLenBits &other) const
  {
    return len <=> other.len;
  }

  constexpr bool operator==(const PrefixLenBits &other) const = default;

  constexpr PrefixLenBits operator+(const uint16_t other_bits) const
  {
    return PrefixLenBits{static_cast<uint16_t>(len + other_bits)};
  }

  constexpr PrefixLenBits(uint16_t len) : len(len) {} // NOLINT
  constexpr PrefixLenBits() : len(0) {}
};

struct SliceCtorT {};

/*! Generic prefix of arbitrary length.
Prefix is broken into pieces of width kBranchBits = 4
*/
template<uint16_t MaxLenBytes> class ByteArrayPrefix {

  static_assert(std::endian::native == std::endian::little,
                "big endian unimplemented");

  constexpr static uint8_t kBranchBits = 4;

  constexpr static uint16_t kWords =
      (MaxLenBytes / 8) + (MaxLenBytes % 8 == 0 ? 0 : 1); // round up

  constexpr static uint64_t kBranchMask =
      (static_cast<uint64_t>(1) << (kBranchBits)) - 1;

  template<uint16_t LenBytes> friend class ByteArrayPrefix;

public:
  constexpr static uint16_t kMaxLenBytes = MaxLenBytes;
  constexpr static uint16_t kMaxLenBits = 8 * kMaxLenBytes;
  constexpr static PrefixLenBits kMaxLen = PrefixLenBits{kMaxLenBits};

  ByteArrayPrefix() : data_() { data_.fill(0); }

  explicit ByteArrayPrefix(const std::array<uint8_t, kMaxLenBytes> &input)
    : data_()
  {
    auto *ptr = reinterpret_cast<uint8_t *>(data_.data());
    std::memcpy(ptr, input.data(), kMaxLenBytes);
  }

  ByteArrayPrefix(const uint8_t *ptr, SliceCtorT) : data_()
  {
    std::memcpy(reinterpret_cast<uint8_t *>(data_.data()), ptr, kMaxLenBytes);
  }

  //! Returns the number of bits that match between this and \a other,
  //! rounded down to the nearest multiple of kBranchBits.
  template<uint16_t OtherMaxLenBytes>
  [[nodiscard]]
  PrefixLenBits
  get_prefix_match_len(const PrefixLenBits &self_len,
                       const ByteArrayPrefix<OtherMaxLenBytes> &other,
                       const PrefixLenBits &other_len) const
  {
    static_assert(kMaxLenBytes <= OtherMaxLenBytes, "other too short");

    uint16_t res = kMaxLenBits;

    for (size_t i = 0; i < kWords; i++) {
      uint64_t local = data_[i] ^ other.data_[i];
      if (local) {

        // count number of trailing zeros, rounding down to nearest kBranchBits
        // units equivalent: word_offset -= (word_offset % kBranchBits);
        uint16_t word_offset = __builtin_ctzll(local) & 0xFFFC;

        res = std::min<uint16_t>((i * 64) + word_offset, res);
        break;
      }
    }
    return std::min<PrefixLenBits>({res, self_len, other_len});
  }

  //! get the kBranchBits bits that follow a specific length point.
  //! I.e. if prefix is 0xABCD, get_branch_bits(4) = C
  [[nodiscard]]
  uint8_t get_branch_bits(const PrefixLenBits &branch_point) const
  {
    assert(branch_point < kMaxLen);

    uint16_t word_idx = branch_point.len / 64;
    uint16_t word_offset = branch_point.len % 64;

    return (data_[word_idx] >> word_offset) & 0xF;
  }

  //! Truncate a prefix to a specific length.  Bits beyond truncate_point
  //! are set to 0.
  void truncate(const PrefixLenBits &truncate_point)
  {
    assert(truncate_point <= kMaxLen);

    if (truncate_point == kMaxLen) {
      return;
    }

    uint16_t word_idx = truncate_point.len / 64;
    uint16_t word_offset = truncate_point.len % 64;

    uint64_t truncate_mask = (static_cast<uint64_t>(1) << word_offset) - 1;

    data_[word_idx] &= truncate_mask;
    for (size_t i = word_idx + 1; i < kWords; i++) {
      data_[i] = 0;
    }
  }

  std::strong_ordering operator<=>(const ByteArrayPrefix &other) const
  {

    if (&other == this)
      return std::strong_ordering::equal;

    auto res = memcmp(reinterpret_cast<const uint8_t *>(data_.data()),
                      reinterpret_cast<const uint8_t *>(other.data_.data()),
                      kMaxLenBytes);
    if (res < 0) {
      return std::strong_ordering::less;
    }
    if (res > 0) {
      return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
  }

  bool operator==(const ByteArrayPrefix &other) const = default;

  uint8_t *underlying_data_ptr(this auto &self)
  {
    return reinterpret_cast<uint8_t *>(self.data_.data());
  }

private:
  std::array<uint64_t, kWords> data_;
};

} // namespace hog
