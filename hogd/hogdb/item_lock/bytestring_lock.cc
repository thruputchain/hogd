#include "hogd/hogdb/item_lock/bytestring_lock.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogdb/close_hash.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "x23/vec.h"

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
BytestringLock::write_committed_change(LockContext const &ctx) const
{
  assert(ctx.dao);
  std::span<const std::byte> v = std::as_bytes(std::span(bytestring_));
  return ctx.dao->write_bytes(ctx.key, v);
}

HogDBStatus<void>
BytestringLock::apply_close_hash_delta(const LockContext &ctx,
                                       LtHash16 &hash) 
{
  assert(ctx.dao);
  auto key = make_contract_data_db_key(ctx.key.as_bytes());
  auto raw = ctx.dao->read_bytes(ctx.key);
  if (raw.has_value()) {
    lthash_remove_item(hash, key, kBytes, *raw);
  }
  x23::Vec<std::byte> marker;
  marker.push_back(std::byte{0x42});
  lthash_add_item(hash, key, kBytes, marker);
  return {};
}

} // namespace hogd
