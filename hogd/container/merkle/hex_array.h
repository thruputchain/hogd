// hex_array.h - provides a hex array abstraction

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include <format>
#include <iomanip>
#include <ios>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

// This is an array of hex characters/digits. The closest equivalent would be an
// [std::vector<std::byte>], but this is cumbersome to use as an [std::byte]
// holds two hex digits, so we cannot index into the vector by hex digit index.
//
// Example:
// HexArray h("1234");
//
// This prints "0x1234":
// std::cout << h << std::endl;
//
// This appends another hex digit to [h]:
// h.append(0x5);
//
// This prints "0x12345":
// std::cout << h << std::endl;
class HexArray {
public:
  static constexpr size_t kMaxNumHexDigits = 128;
  static_assert(kMaxNumHexDigits % 2 == 0);

  HexArray() { init(); }

  HexArray(const std::string &s)
  {
    // TODO(jhumphri): Gracefully handle this rather than crashing. This is
    // mostly a concern for when we verify a proof--we do not want an incorrect
    // path that is too long to crash the node.
    CHECK_LE(s.size(), kMaxNumHexDigits);

    init();

    for (size_t i = 0; i < s.size(); i++) {
      std::string substr = s.substr(i, 1);
      auto b = static_cast<std::byte>(
          std::stoul(substr, /*pos=*/nullptr, /*base=*/16));
      set_helper(i, b);
    }
    size_ = s.size();
  }

  HexArray(const std::vector<std::byte> &bytes)
  {
    CHECK_LE(bytes.size(), kArrayLength);

    init();

    memcpy(backing_store_.data(), bytes.data(), bytes.size());
    size_ = bytes.size() * 2;
  }

  HexArray(const SHA256Hash &hash)
  {
    CHECK_LE(hash.size(), kArrayLength);

    init();

    memcpy(backing_store_.data(), &hash, hash.size() * sizeof(hash[0]));
    size_ = hash.size() * 2;
  }

  bool operator==(const HexArray &a) const
  {
    if (size_ != a.size()) {
      return false;
    }
    bool backing_store_equal =
        std::equal(backing_store_.begin(), backing_store_.begin() + (size_ / 2),
                   a.backing_store_.begin());
    if (size_ % 2) {
      // Check the last hex digit.
      backing_store_equal =
          backing_store_equal && (get(size_ - 1) == a.get(size_ - 1));
    }
    return backing_store_equal;
  }

  bool operator<(const HexArray &a) const
  {
    if (size_ < a.size()) {
      return true;
    }
    if (size_ > a.size()) {
      return false;
    }

    CHECK_EQ(size_, a.size());
    // find the hex digit where the two arrays diverge.
    for (size_t i = 0; i < size_; i++) {
      if (get(i) < a.get(i)) {
        return true;
      }
      if (get(i) > a.get(i)) {
        return false;
      }
      // get(i) == a.get(i), so continue looping.
    }
    return false;
  }

  std::string str() const
  {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size_; i++) {
      std::byte b = get(i);
      oss << std::setw(1) << static_cast<int>(b);
    }
    return oss.str();
  }

  friend std::ostream &operator<<(std::ostream &os, const HexArray &h)
  {
    return os << h.str();
  }

  std::byte get(size_t idx) const
  {
    CHECK_LT(idx, size_);

    if (idx % 2 == 0) {
      return static_cast<std::byte>(backing_store_[idx / 2] >> 4);
    }
    return static_cast<std::byte>(backing_store_[idx / 2] & std::byte{0x0F});
  }

  void set(size_t idx, std::byte b)
  {
    CHECK_LT(idx, size_);

    set_helper(idx, b);
  }

  void append(std::byte b)
  {
    CHECK_LE(size_ + 1, kMaxNumHexDigits);

    set_helper(size_, b);
    size_++;
  }

  // TODO(jhumphri): Optimize this.
  void append(const HexArray &to_append)
  {
    CHECK_LE(size_ + to_append.size(), kMaxNumHexDigits);

    for (size_t i = 0; i < to_append.size(); i++) {
      set_helper(size_ + i, to_append.get(i));
    }
    size_ += to_append.size();
  }

  void remove_from_back(size_t to_remove)
  {
    CHECK_LE(to_remove, size_);
    size_ -= to_remove;
  }

  HexArray get_subarray(size_t start, size_t end) const
  {
    CHECK_LE(start, size_);
    CHECK_LE(end, size_);
    HexArray h;
    for (size_t i = start; i < end; i++) {
      h.append(get(i));
    }
    return h;
  }

  std::vector<std::byte> to_byte_vector() const
  {
    std::vector<std::byte> ret;
    std::copy_n(backing_store_.begin(), (size_ + 1) / 2,
                std::back_inserter(ret));
    if (size_ % 2) {
      // If the size is odd, zero out the last 4 bits to ensure we consistently
      // return the same array. Otherwise, we could end up returning garbage in
      // the last 4 bits, which could vary between invocations of
      // [ToByteArray()].
      ret.back() &= std::byte{0xF0};
    }
    return ret;
  }

  void clear() { size_ = 0; }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0U; }

private:
  // [kMaxNumHexDigits] is always even (see the static_assert() above).
  static constexpr size_t kArrayLength = kMaxNumHexDigits / 2;

  void init()
  {
    memset(backing_store_.data(), 0,
           backing_store_.size() * sizeof(backing_store_[0]));
    size_ = 0;
  }

  void set_helper(size_t idx, std::byte b)
  {
    CHECK_LE(idx, kMaxNumHexDigits);

    if (idx % 2 == 0) {
      backing_store_[idx / 2] &= std::byte{0x0F};
      backing_store_[idx / 2] |= (b << 4);
    }
    else {
      backing_store_[idx / 2] &= std::byte{0xF0};
      backing_store_[idx / 2] |= (b & std::byte{0x0F});
    }
  }

  std::array<std::byte, kMaxNumHexDigits> backing_store_;
  size_t size_;
};

} // namespace hogd

// We need to define the formatter in the [std] namespace.
template<> struct std::formatter<hogd::HexArray> : std::formatter<std::string> {
  auto format(const hogd::HexArray &h, std::format_context &ctx) const
  {
    std::ostringstream oss;
    oss << h;
    return std::formatter<std::string>::format(oss.str(), ctx);
  }
};
