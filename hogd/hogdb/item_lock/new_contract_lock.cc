#include "hogd/hogdb/item_lock/new_contract_lock.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "db_conflict.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

HogDBStatus<int128_t>
NewContractLock::try_atomic_change_lock(const NewContractData *before,
                                        const NewContractData *after,
                                        const LockContext &ctx)
{
  assert(before == nullptr || before->new_contract() == contract_);

  if (after != nullptr && after->new_contract() != contract_) {
    return ctx.conflict(DbConflictCode::kDbConflictValueMismatch);
  }

  // pay on contract deployment, not contract create
  return 0;
}

HogDBStatus<void>
NewContractLock::stage_committed_changes(const LockContext &ctx,
                                         ChangeSet &changes) const
{
  SHA256Hash code_hash = ctx.key.get_contract_code_hash();
  auto buf = x23::serialize(contract_);
  // Large rows are staged as multiple chunk items; small rows keep the
  // legacy single-item layout. See for_each_wasm_code_item.
  for_each_wasm_code_item(code_hash, std::as_bytes(std::span(buf)),
                          [&changes](std::span<const std::byte> key,
                                     uint8_t type,
                                     std::span<const std::byte> value) {
                            changes.put(key, type, value);
                          });
  return {};
}

} // namespace hogd
