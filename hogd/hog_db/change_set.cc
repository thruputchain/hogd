#include "hogd/hog_db/change_set.h"

#include <array>
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
  auto item = alloc_->store_item(key, type, value);
  stage_item(item, key.size(), type);
}

void
ChangeSet::stage_item(std::span<const std::byte> item, size_t key_len,
                      uint8_t type)
{
  auto key = item.first(key_len);
  auto value = item.subspan(key_len + 1);

  MaterializedChange change;
  change.op = MaterializedChange::Op::kPut;
  change.type = type;
  change.key_len = key.size();
  change.item = item;

  if (compute_hash_ && is_state_item(key)) {
    auto &hash = *hash_delta_;
    hash.add(key, type, value);
    remove_old_hash(key);
  }

  changes_->push_back(change);
}

void
ChangeSet::remove(std::span<const std::byte> key)
{
  auto &alloc = *alloc_;
  auto &changes = *changes_;

  if (compute_hash_ && is_state_item(key)) {
    if (!remove_old_hash(key))
      return;
  }
  else if (auto type = dao_->get_ramdb().get_type(key); !type) {
    CHECK(type.error() == HogDbCode::kNotFound);
    return;
  }

  MaterializedChange change;
  change.op = MaterializedChange::Op::kRemove;
  change.key_len = key.size();
  change.item = alloc.store(key);

  changes.push_back(change);
}

bool
ChangeSet::remove_old_hash(std::span<const std::byte> key)
{
  // Scalar rows need no owning allocation. Hash after releasing the bucket
  // lock; large rows retain the existing lifetime-safe owning read.
  std::array<std::byte, 64> bytes;
  auto &db = dao_->get_ramdb();
  if (auto old = db.get_into(key, bytes)) {
    hash_delta_->remove(key, old->first, std::span(bytes).first(old->second));
    return true;
  }
  else if (old.error() != HogDbCode::kTooBig) {
    CHECK(old.error() == HogDbCode::kNotFound);
    return false;
  }
  auto old = db.get(key);
  if (!old) {
    CHECK(old.error() == HogDbCode::kNotFound);
    return false;
  }
  hash_delta_->remove(key, old->type, old->value);
  return true;
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
