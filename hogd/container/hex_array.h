// hex_array.h - provides a hex array abstraction

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/check.h"
#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

// TODO: Describe the class.
//
// Example:
// TODO: Provide example.
class HexArray {
public:
  static constexpr size_t kMaxNumHexDigits = 66;
  static_assert(kMaxNumHexDigits % 2 == 0);

  HexArray() { Init(); }

  HexArray(const std::string &s)
  {
    CHECK_LE(s.size(), kMaxNumHexDigits);

    Init();

    for (size_t i = 0; i < s.size(); i++) {
      std::string substr = s.substr(i, 1);
      std::byte b = static_cast<std::byte>(
          std::stoul(substr, /*pos=*/nullptr, /*base=*/16));
      SetHelper(i, b);
    }
    size_ = s.size();
  }

  HexArray(const std::vector<std::byte> &bytes)
  {
    CHECK_LE(bytes.size(), kArrayLength);

    Init();

    memcpy(backing_store_.data(), bytes.data(), bytes.size());
    size_ = bytes.size() * 2;
  }

  HexArray(const SHA256Hash &hash)
  {
    CHECK_LE(hash.size(), kArrayLength);

    Init();

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
          backing_store_equal && (Get(size_ - 1) == a.Get(size_ - 1));
    }
    return backing_store_equal;
  }

  bool operator<(const HexArray &a) const
  {
    if (size_ < a.size()) {
      return true;
    }
    else if (size_ > a.size()) {
      return false;
    }

    CHECK_EQ(size_, a.size());
    // Find the hex digit where the two arrays diverge.
    for (size_t i = 0; i < size_; i++) {
      if (Get(i) < a.Get(i)) {
        return true;
      }
      else if (Get(i) > a.Get(i)) {
        return false;
      }
      else {
        // Get(i) == a.Get(i), so continue looping.
      }
    }
    return false;
  }

  std::string str() const
  {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size_; i++) {
      std::byte b = Get(i);
      oss << std::setw(1) << static_cast<int>(b);
    }
    return oss.str();
  }

  std::byte Get(size_t idx) const
  {
    CHECK_LT(idx, size_);

    if (idx % 2 == 0) {
      return static_cast<std::byte>(backing_store_[idx / 2] >> 4);
    }
    else {
      return static_cast<std::byte>(backing_store_[idx / 2] & std::byte{0x0F});
    }
  }

  void Set(size_t idx, std::byte b)
  {
    CHECK_LT(idx, size_);

    SetHelper(idx, b);
  }

  void Append(std::byte b)
  {
    CHECK_LE(size_ + 1, kMaxNumHexDigits);

    SetHelper(size_, b);
    size_++;
  }

  // TODO: Optimize this.
  void Append(const HexArray &to_append)
  {
    CHECK_LE(size_ + to_append.size(), kMaxNumHexDigits);

    for (size_t i = 0; i < to_append.size(); i++) {
      SetHelper(size_ + i, to_append.Get(i));
    }
    size_ += to_append.size();
  }

  void RemoveFromBack(size_t to_remove)
  {
    CHECK_LE(to_remove, size_);
    size_ -= to_remove;
  }

  HexArray GetSubarray(size_t start, size_t end) const
  {
    CHECK_LE(start, size_);
    CHECK_LE(end, size_);
    HexArray h;
    for (size_t i = start; i < end; i++) {
      h.Append(Get(i));
    }
    return h;
  }

  std::vector<std::byte> ToByteVector() const
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

  // TODO: Add a test for this.
  void Clear() { size_ = 0; }

  size_t size() const { return size_; }
  bool empty() const { return !size_; }

private:
  // [kMaxNumHexDigits] is always even (see the static_assert() above).
  static constexpr size_t kArrayLength = kMaxNumHexDigits / 2;

  void Init()
  {
    memset(backing_store_.data(), 0,
           backing_store_.size() * sizeof(backing_store_[0]));
    size_ = 0;
  }

  void SetHelper(size_t idx, std::byte b)
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
