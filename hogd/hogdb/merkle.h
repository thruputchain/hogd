// merkle.h - Merkle trie helpers.

#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/container/merkle/merkle_trie.h"

namespace hogd {

class MerkleTrieHelpers {
public:
  MerkleTrieHelpers() = delete;

  static std::span<const std::byte>
  add_type_to_merkle_key(std::span<const std::byte> &key, uint8_t type)
  {
    static PerThread<std::vector<std::byte>> backing_store;

    backing_store->clear();
    backing_store->insert(backing_store->end(), key.begin(), key.end());
    backing_store->push_back(static_cast<std::byte>(type));
    return std::span<const std::byte>(*backing_store);
  }

  static HogDBStatus<void>
  update_kv_pair_in_trie(MerkleTriePrefix &trie, const ItemKey &key,
                         uint8_t type, const std::span<const std::byte> &value)
  {
    std::span<const std::byte> raw_key = key.get_address().as_bytes();
    std::span<const std::byte> k = add_type_to_merkle_key(raw_key, type);
    if (!trie.insert_or_update(k, value)) {
      return HogDBError(HogDBCode::kInternal);
    }
    return {};
  }

  static HogDBStatus<void> remove_kv_pair_from_trie(MerkleTriePrefix &trie,
                                                    const ItemKey &key,
                                                    uint8_t type)
  {
    std::span<const std::byte> raw_key = key.get_address().as_bytes();
    std::span<const std::byte> k = add_type_to_merkle_key(raw_key, type);
    if (!trie.remove(k)) {
      return HogDBError(HogDBCode::kNotFound);
    }
    return {};
  }
};

} // namespace hogd
