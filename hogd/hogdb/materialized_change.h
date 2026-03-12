// -*-C++-*-

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/crypto/lthash16.h"

namespace hogd {

// A single materialized DAO mutation with pre-built hash elements.
// Byte data (keys, values, hash elements) stored as spans pointing into
// a BumpAllocator arena. Zero per-record heap allocations.
struct MaterializedChange {
  enum class Op : uint8_t { kPut, kRemove };

  Op op;
  uint8_t type = 0;
  std::span<const std::byte> key;
  std::span<const std::byte> value;

  // Pre-built hash elements: [key + type_byte + value].
  // Empty if is_state_item(key) is false (e.g. HogAge).
  std::span<const std::byte> hash_remove;
  std::span<const std::byte> hash_add;

  void apply_hash(LtHash16 &hash) const
  {
    if (!hash_remove.empty()) {
      hash.remove(hash_remove);
    }
    if (!hash_add.empty()) {
      hash.add(hash_add);
    }
  }
};

} // namespace hogd
