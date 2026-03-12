// -*-C++-*-

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/base/perthread.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/hogdb/bump_allocator.h"
#include "hogd/hogdb/materialized_change.h"

namespace hogd {

class RAMDB;
class HogDBDAO;

// Encapsulates the prepare/hash/flush lifecycle for block finalization.
// Owns per-thread bump allocators and change vectors. Lock code and BlockTX
// interact through Prepared's public API.
class Prepared {
  friend class HogDBDAO;
  explicit Prepared(RAMDB *ramdb);

public:
  // Called by locks during Phase A (parallel over locks).
  // Reads old value from RAMDB internally for hash element construction.
  void put(std::span<const std::byte> key, uint8_t type,
           std::span<const std::byte> value);
  void remove(std::span<const std::byte> key);

  // Reserve capacity in the calling thread's change vector.
  void reserve(size_t n);

  // Compute the hash delta from all accumulated changes.
  // Signals that no more changes will be added.
  LtHash16 close();

private:
  RAMDB *ramdb_;
  PerThread<ByteArena> alloc_;
  PerThread<std::vector<MaterializedChange>> changes_;
};

} // namespace hogd
