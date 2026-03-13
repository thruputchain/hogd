// -*-C++-*-

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/crypto/lthash16.h"
#include "hogd/hogdb/hogdb_dao.h"

namespace hogd {

// A single materialized DAO mutation. Byte data (keys, values) stored as
// spans pointing into a ByteArena. Zero per-record heap allocations.
//
// item and old_item store contiguous [key + type_byte + value] layouts.
// key() and value() are accessor methods that derive spans from item/old_item
// using key_len.
struct MaterializedChange {
  enum class Op : uint8_t { kPut, kRemove };

  Op op;
  uint8_t type = 0;
  size_t key_len = 0;
  std::span<const std::byte> item;     // new: [key + type_byte + value]
  std::span<const std::byte> old_item; // previous: [key + type_byte + value]

  std::span<const std::byte> key() const
  {
    auto const &src = item.empty() ? old_item : item;
    return src.subspan(0, key_len);
  }

  std::span<const std::byte> value() const { return item.subspan(key_len + 1); }

  void update_hash(LtHash16 &hash) const
  {
    if (!is_state_item(key())) {
      return;
    }
    if (!old_item.empty()) {
      hash.remove(old_item);
    }
    if (!item.empty()) {
      hash.add(item);
    }
  }
};

} // namespace hogd
