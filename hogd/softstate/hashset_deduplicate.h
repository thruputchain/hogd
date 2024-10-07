#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_set>

#include "hogd/crypto/crypto.h"
#include "hogd/softstate/atomic_hashset_node.h"

#include "types.hh"

namespace hogd {

struct Hasher {
  Siphash24Key key_ = siphash24_keygen();

  size_t operator()(const SHA256Hash &entry) const noexcept
  {
    return siphash24(std::as_bytes(std::span(entry)), key_);
  }
};

class HashsetDeduplicatorSimple {
public:
  bool try_insert(SHA256Hash const &entry);

  void remove(SHA256Hash const &entry);

private:
  std::unordered_set<SHA256Hash, Hasher> set_;
  std::mutex mtx_;
};

class HashsetDeduplicatorSimpleV2 {
  struct InternalEntry {
    SHA256Hash hash_;
    uint64_t cached_shorthash_;

    bool operator==(InternalEntry const &other) const
    {
      return other.hash_ == hash_;
    }
  };

  struct InternalHasher {
    size_t operator()(const InternalEntry &entry) const noexcept
    {
      return entry.cached_shorthash_;
    }
  };

public:
  bool try_insert(SHA256Hash const &entry);

  void remove(SHA256Hash const &entry);

private:
  Hasher hasher_;

  std::unordered_set<InternalEntry, InternalHasher> set_;
  std::mutex mtx_;
};

class AtomicHashsetDeduplicator {
public:
  AtomicHashsetDeduplicator() : root_(0, 0) { root_.commit_owning(); }

  bool try_insert(SHA256Hash const &entry);
  void remove(SHA256Hash const &entry);

private:
  AtomicHashsetNode root_;
  Hasher hasher_;
};

typedef HashsetDeduplicatorSimple HashsetDeduplicator;

} // namespace hogd
