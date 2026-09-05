#include "hogd/hog_db/change_set.h"

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/hog_db/byte_arena.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/materialized_change.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

ChangeSet::ChangeSet(HogDbDAO *dao, bool compute_hash)
  : dao_(dao), compute_hash_(compute_hash)
{}

void // NOLINTNEXTLINE(misc-include-cleaner)
ChangeSet::put(std::span<const std::byte> key, uint8_t type,
               std::span<const std::byte> value)
{
  auto &alloc = *alloc_;
  auto &changes = *changes_;

  auto item = alloc.store_item(key, type, value);

  MaterializedChange change;
  change.op = MaterializedChange::Op::kPut;
  change.type = type;
  change.key_len = key.size();
  change.item = item;

  if (compute_hash_ && is_state_item(key)) {
    auto &hash = *hash_delta_;
    hash.add(key, type, value);
    auto old = dao_->get_ramdb().get(key);
    if (old.has_value()) {
      hash.remove(key, old->type, old->value);
    }
  }

  changes.push_back(change);
}

void
ChangeSet::remove(std::span<const std::byte> key)
{
  auto &alloc = *alloc_;
  auto &changes = *changes_;

  auto old = dao_->get_ramdb().get(key);
  if (!old.has_value()) {
    CHECK(old.error() == DBCode::kNotFound);
    return;
  }

  if (compute_hash_ && is_state_item(key)) {
    hash_delta_->remove(key, old->type, old->value);
  }

  MaterializedChange change;
  change.op = MaterializedChange::Op::kRemove;
  change.key_len = key.size();
  change.item = alloc.store(key);

  changes.push_back(change);
}

void // NOLINTNEXTLINE(misc-include-cleaner)
ChangeSet::reserve(size_t n)
{
  changes_->reserve(n);
}

LtHash16
ChangeSet::close()
{
  // Hash deltas were computed inline during put()/remove(), one per thread.
  // Merge all per-thread deltas into a single result.
  LtHash16 delta;
  auto ul = hash_delta_.lock_threads();
  hash_delta_.for_each(ul,
                       [&delta](const LtHash16 &partial) { delta += partial; });
  return delta;
}

} // namespace hogd
