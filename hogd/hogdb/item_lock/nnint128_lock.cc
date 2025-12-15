#include "hogd/hogdb/item_lock/nnint128_lock.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <span>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

HogDBStatus<int128_t>
NNInt128Lock::try_atomic_change_lock(const NNInt128 *before,
                                     const NNInt128 *after,
                                     const StorageCostModel *cost_model,
                                     const LockContext & /*ctx*/)
{
  assert(before == nullptr || before->base() == base_);

  if (after != nullptr && after->base() != base_) {
    return HogDBError{HogDBCode::kConflict};
  }

  int128_t add_before = before == nullptr ? 0 : before->plus();
  int128_t sub_before = before == nullptr ? 0 : before->minus();
  int128_t add_after = after == nullptr ? 0 : after->plus();
  int128_t sub_after = after == nullptr ? 0 : after->minus();

  assert(add_before >= 0);
  assert(add_after >= 0);
  assert(sub_before <= 0);
  assert(sub_after <= 0);

  // these calculations can only overflow if sub_after == 0 and sub_before ==
  // int128_min However, it should never be possible to acquire a sub_before ==
  // int128_min lock, as that would require subtracing int128_min from an int128
  // base value without going negative, which is impossible (base max is
  // int128_max, and int128_max - int128_min is -1)
  assert(sub_before != std::numeric_limits<int128_t>::min());
  int128_t add_delta = add_after - add_before;
  int128_t sub_delta = sub_after - sub_before;

  // Once we reserve the adds and subtracts, it must be safe to add to the
  // delta.
  int128_t delta_before = before == nullptr ? 0 : before->delta();
  int128_t delta_after = after == nullptr ? 0 : after->delta();
  auto [delta_delta, overflow] = overflowing_sub(delta_after, delta_before);
  assert(!overflow);

  assert(add_before >= delta_before);
  assert(sub_before <= delta_before);
  assert(add_after >= delta_after);
  assert(sub_after <= delta_after);

  int128_t add_lock = add_delta > 0 ? add_delta : 0;
  int128_t sub_lock = sub_delta < 0 ? sub_delta : 0;
  int128_t add_release = add_delta > 0 ? 0 : add_delta;
  int128_t sub_release = sub_delta < 0 ? 0 : sub_delta;

  assert(add_lock + add_release == add_delta);
  assert(sub_lock + sub_release == sub_delta);

  if (!try_reserve_add_capacity(add_lock)) {
    return HogDBError{HogDBCode::kConflict};
  }
  if (!try_reserve_sub_capacity(sub_lock)) {
    // release the locked add capacity
    release_add_capacity(add_lock);
    return HogDBError{HogDBCode::kConflict};
  }

  // no more locks to acquire
  if (delta_delta != 0) {
    delta_.fetch_add(delta_delta, std::memory_order_relaxed);
  }

  release_add_capacity(-add_release);
  // negation is safe
  assert(sub_release != std::numeric_limits<int128_t>::min());
  release_sub_capacity(-sub_release);

  return get_storage_cost_of_committed_value(cost_model);
}

HogDBStatus<void>
NNInt128Lock::write_committed_change(LockContext const &ctx,
                                     MerkleTriePrefix &trie) const
{

  auto delta = delta_.load();
  auto [value, of] = overflowing_add(base_, delta);
  CHECK(!of);

  assert(ctx.dao);
  HogDBStatus<void> ret = ctx.dao->write_nnint128(ctx.key, value);
  if (ret) {
    std::span<std::byte> value_span(reinterpret_cast<std::byte *>(&value),
                                    sizeof(value));
    // [MerkleTrieHelpers::update_kv_pair_in_trie] will always succeed as
    // [trie.insert_or_update()] only fails if we attempt to insert an empty
    // key.
    CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, ctx.key, kNNInt128,
                                                    value_span)
              .has_value());
  }
  return ret;
}

} // namespace hogd
