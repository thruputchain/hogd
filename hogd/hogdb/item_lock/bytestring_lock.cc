#include "hogd/hogdb/item_lock/bytestring_lock.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<int128_t>
BytestringLock::try_atomic_change_lock(const Bytes *before, const Bytes *after,
                                       const LockContext &ctx)
{
  assert(before == nullptr || before->value_view() == bytestring_);
  if (after != nullptr && after->value_view() != bytestring_) {
    return HogDBError{HogDBCode::kConflict};
  }
  return get_storage_cost_of_committed_value(ctx.cost_model);
}

HogDBStatus<void>
BytestringLock::write_committed_change(LockContext const &ctx,
                                       MerkleTriePrefix &trie) const
{
  assert(ctx.dao);
  std::span<const std::byte> v = std::as_bytes(std::span(bytestring_));
  HogDBStatus<void> ret = ctx.dao->write_bytes(ctx.key, v);
  if (ret) {
    // [MerkleTrieHelpers::update_kv_pair_in_trie] will always succeed as
    // [trie.insert_or_update()] only fails if we attempt to insert an empty
    // key.
    CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, ctx.key, kBytes, v)
              .has_value());
  }
  return ret;
}

} // namespace hogd
