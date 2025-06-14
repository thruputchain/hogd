// merkle.h - provides a merkle patricia trie

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cassert>
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
  static constexpr size_t kMaxNumHexDigits = 60;
  static_assert(kMaxNumHexDigits % 2 == 0);

  HexArray() { Init(); }

  HexArray(const std::string &s)
  {
    Init();

    for (size_t i = 0; i < s.size(); i++) {
      std::string substr = s.substr(i, 1);
      std::byte b = static_cast<std::byte>(
          std::stoul(substr, /*pos=*/nullptr, /*base=*/16));
      Set(i, b);
    }
    size_ = s.size();
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
    if (idx % 2 == 0) {
      return static_cast<std::byte>(backing_store_[idx / 2] >> 4);
    }
    else {
      return static_cast<std::byte>(backing_store_[idx / 2] & std::byte{0x0F});
    }
  }

  void Set(size_t idx, std::byte b)
  {
    if (idx % 2 == 0) {
      backing_store_[idx / 2] &= std::byte{0x0F};
      backing_store_[idx / 2] |= (b << 4);
    }
    else {
      backing_store_[idx / 2] &= std::byte{0xF0};
      backing_store_[idx / 2] |= (b & std::byte{0x0F});
    }
  }

  void Append(std::byte b)
  {
    assert(size_ + 1 <= kMaxNumHexDigits);
    Set(size_, b);
    size_++;
  }

  // TODO: Optimize this.
  void Append(const HexArray &to_append)
  {
    assert(size_ + to_append.size() <= kMaxNumHexDigits);
    for (size_t i = 0; i < to_append.size(); i++) {
      Set(size_ + i, to_append.Get(i));
    }
    size_ += to_append.size();
  }

  void RemoveFromBack(size_t to_remove)
  {
    assert(to_remove <= size_);
    size_ -= to_remove;
  }

  size_t size() const { return size_; }

private:
  // [kMaxNumHexDigits] is always even (see the static_assert() above).
  static constexpr size_t kArrayLength = kMaxNumHexDigits / 2;

  void Init()
  {
    memset(backing_store_.data(), 0,
           backing_store_.size() * sizeof(backing_store_[0]));
    size_ = 0;
  }

  std::array<std::byte, kMaxNumHexDigits> backing_store_;
  size_t size_;
};

} // namespace hogd
