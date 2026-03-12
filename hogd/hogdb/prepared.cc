#include "hogd/hogdb/prepared.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "hogd/base/perthread.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/hogdb/bump_allocator.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/materialized_change.h"

namespace hogd {

Prepared::Prepared(RAMDB *ramdb) : ramdb_(ramdb) {}

void
Prepared::put(std::span<const std::byte> key, uint8_t type,
              std::span<const std::byte> value)
{
  auto &alloc = *alloc_;
  auto &changes = *changes_;

  auto stored_key = alloc.store(key);
  auto stored_value = alloc.store(value);

  MaterializedChange change;
  change.op = MaterializedChange::Op::kPut;
  change.type = type;
  change.key = stored_key;
  change.value = stored_value;

  if (is_state_item(key)) {
    auto old = ramdb_->get(key);
    if (old.has_value()) {
      change.hash_remove =
          alloc.build_hash_element(stored_key, old->type, old->value);
    }
    change.hash_add = alloc.build_hash_element(stored_key, type, stored_value);
  }

  changes.push_back(change);
}

void
Prepared::remove(std::span<const std::byte> key)
{
  auto &alloc = *alloc_;
  auto &changes = *changes_;

  auto stored_key = alloc.store(key);

  MaterializedChange change;
  change.op = MaterializedChange::Op::kRemove;
  change.key = stored_key;

  if (is_state_item(key)) {
    auto old = ramdb_->get(key);
    if (old.has_value()) {
      change.hash_remove =
          alloc.build_hash_element(stored_key, old->type, old->value);
    }
  }

  changes.push_back(change);
}

void
Prepared::reserve(size_t n)
{
  changes_->reserve(n);
}

LtHash16
Prepared::close()
{
  LtHash16 delta;
  auto ul = changes_.lock_threads();
  changes_.for_each(
      ul, [&delta](const std::vector<MaterializedChange> &changes) {
        for (const auto &change : changes) {
          change.apply_hash(delta);
        }
      });
  return delta;
}

} // namespace hogd
