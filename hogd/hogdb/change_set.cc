#include "hogd/hogdb/change_set.h"

#include <cstddef>
#include <cstdint> // NOLINT(misc-include-cleaner)
#include <span>
#include <vector>

#include "hogd/crypto/lthash16.h"
#include "hogd/hogdb/byte_arena.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/materialized_change.h"

namespace hogd {

ChangeSet::ChangeSet(HogDBDAO *dao) : dao_(dao)
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

  if (is_state_item(key)) {
    auto old = dao_->get_ramdb().get(key);
    if (old.has_value()) {
      change.old_item = alloc.store_item(key, old->type, old->value);
    }
  }

  changes.push_back(change);
}

void
ChangeSet::remove(std::span<const std::byte> key)
{
  auto &alloc = *alloc_;
  auto &changes = *changes_;

  // Always read old value so key() can derive the key from old_item.
  auto old = dao_->get_ramdb().get(key);
  if (!old.has_value()) {
    // Key doesn't exist in RAMDB — nothing to remove or hash-update.
    return;
  }

  MaterializedChange change;
  change.op = MaterializedChange::Op::kRemove;
  change.key_len = key.size();
  change.old_item = alloc.store_item(key, old->type, old->value);

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
  LtHash16 delta;
  auto ul = changes_.lock_threads();
  changes_.for_each(ul,
                    [&delta](const std::vector<MaterializedChange> &changes) {
                      for (const auto &change : changes) {
                        change.update_hash(delta);
                      }
                    });
  return delta;
}

} // namespace hogd
