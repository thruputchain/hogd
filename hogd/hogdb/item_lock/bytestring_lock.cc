#include "hogd/hogdb/item_lock/bytestring_lock.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

HogDBStatus<int128_t>
BytestringLock::try_atomic_change_lock(const Bytes *before, const Bytes *after,
                                       const StorageCostModel *cost_model)
{
  assert(before == nullptr || before->value_view() == bytestring_);
  if (after != nullptr && after->value_view() != bytestring_) {
    return HogDBError{HogDBCode::kConflict};
  }
  return get_storage_cost_of_committed_value(cost_model);
}

HogDBStatus<void>
BytestringLock::write_committed_change(ItemKey const &key, HogDBDAO &dao,
                                       MerkleTriePrefix &trie) const
{
  std::span<const std::byte> v = std::as_bytes(std::span(bytestring_));
  HogDBStatus<void> ret = dao.write_bytes(key, v);
  if (ret) {
    // [MerkleTrieHelpers::update_kv_pair_in_trie] will always succeed as
    // [trie.insert_or_update()] only fails if we attempt to insert an empty
    // key.
    CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, key, kBytes, v)
              .has_value());
  }
  return ret;
}

} // namespace hogd
