// -*-C++-*-

#pragma once

#include <cstddef>
#include <span>

#include "hogd/base/log.h"
#include "hogd/base/types.h"

namespace hogd {

// A single materialized DAO mutation. Byte data (keys, values) stored as
// spans pointing into a ByteArena. Zero per-record heap allocations.
//
// For kPut, item stores contiguous [key + type_byte + value].
// For kRemove, item stores just the key bytes.
// key() and value() are accessor methods that derive spans using key_len.
struct MaterializedChange {
  enum class Op : uint8_t { kPut, kRemove };

  Op op;
  uint8_t type = 0;
  size_t key_len = 0;
  std::span<const std::byte> item;

  std::span<const std::byte> key() const { return item.subspan(0, key_len); }

  std::span<const std::byte> value() const
  {
    CHECK(op == Op::kPut);
    return item.subspan(key_len + 1);
  }
};

} // namespace hogd
