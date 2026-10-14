// -*-C++-*-

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "hogd/base/perthread.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/hog_db/byte_arena.h"
#include "hogd/hog_db/materialized_change.h"

namespace hogd {

class HogDbDAO;

// Encapsulates the stage/hash/flush lifecycle for block finalization.
// Owns per-thread arenas and change vectors. Lock code and BlockTX
// interact through ChangeSet's public API.
class ChangeSet {
  friend class HogDbDAO;
  ChangeSet(HogDbDAO *dao, bool compute_hash);

public:
  // Called by locks during block close to stage changes.
  // Reads old value from RAMDB for item construction.
  void put(std::span<const std::byte> key, uint8_t type,
           std::span<const std::byte> value);
  void remove(std::span<const std::byte> key);

  // Template overloads that serialize key/value via std::as_bytes(std::span).
  // Explicit std::span<const std::byte> casts avoid infinite recursion when
  // K is a fixed-extent span of bytes (e.g. std::span<const std::byte, 33>).
  template<typename K, typename V>
  void put(const K &key, uint8_t type, const V &value)
  {
    put(std::span<const std::byte>(std::as_bytes(std::span(key))), type,
        std::span<const std::byte>(std::as_bytes(std::span(value))));
  }

  template<typename K>
  void put(const K &key, uint8_t type, std::span<const std::byte> value)
  {
    put(std::span<const std::byte>(std::as_bytes(std::span(key))), type, value);
  }

  template<typename K> void remove(const K &key)
  {
    remove(std::span<const std::byte>(std::as_bytes(std::span(key))));
  }

  // Reserve capacity in the calling thread's change vector.
  void reserve(size_t n);

  // Finalize the ChangeSet and return the hash delta.
  // Hash deltas are accumulated inline during put()/remove(); close()
  // merges the per-thread partial hashes.
  LtHash16 close();

private:
  HogDbDAO *dao_;
  bool compute_hash_;
  PerThread<ByteArena> alloc_;
  PerThread<std::vector<MaterializedChange>> changes_;
  PerThread<LtHash16> hash_delta_;
};

} // namespace hogd
