// merkle.h - Merkle trie helpers.

#pragma once

#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/status.h"

namespace hogd {

class MerkleTrieHelpers {
public:
  // This class just contains static methods, so we do not allow you to
  // construct an instance of the class.
  MerkleTrieHelpers() = delete;

  // Creates the key to insert into the Merkle trie from [key] and [type]. We
  // want to include [type] in the key because values with the same key could
  // have different types. Two tries that each contain the same KV pair, but
  // with different value types, should have different root hashes.
  static std::span<const std::byte>
  add_type_to_merkle_key(std::span<const std::byte> const &key, uint8_t type)
  {
    static PerThread<std::vector<std::byte>> backing_store;

    backing_store->clear();
    backing_store->insert(backing_store->end(), key.begin(), key.end());
    backing_store->push_back(static_cast<std::byte>(type));
    return std::span<const std::byte>(*backing_store);
  }

  // Inserts or updates a KV pair.
  static HogDBStatus<void>
  update_kv_pair_in_trie(MerkleTriePrefix &trie, const ItemKey &key,
                         uint8_t type, const std::span<const std::byte> &value)
  {
    std::span<const std::byte> raw_key = key.as_bytes();
    std::span<const std::byte> k = add_type_to_merkle_key(raw_key, type);
    if (!trie.insert_or_update(k, value)) {
      return HogDBError(HogDBCode::kInternal);
    }
    return {};
  }

  // Removes a KV pair.
  static HogDBStatus<void> remove_kv_pair_from_trie(MerkleTriePrefix &trie,
                                                    const ItemKey &key,
                                                    uint8_t type)
  {
    std::span<const std::byte> raw_key = key.as_bytes();
    std::span<const std::byte> k = add_type_to_merkle_key(raw_key, type);
    if (!trie.remove(k)) {
      return HogDBError(HogDBCode::kNotFound);
    }
    return {};
  }
};

} // namespace hogd
