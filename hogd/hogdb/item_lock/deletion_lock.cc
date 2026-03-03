#include "hogd/hogdb/item_lock/deletion_lock.h"

#include <cassert>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/close_hash.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/retrocausal_queue_lock.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<void>
DeletionLock::write_committed_change(LockContext const &ctx)
{
  int8_t type = 0;
  if (ctx.key.is_address_auth_data()) {
    type = 0;
  }
  else if (ctx.key.is_contract_data()) {
    assert(ctx.dao);
    auto type_ret = ctx.dao->get_item_type(ctx.key);
    if (!type_ret) {
      // Deleting a non-existent key is a no-op.
      if (type_ret.error() == HogDBCode::kNotFound) {
        return {};
      }
      return type_ret.error();
    }
    type = type_ret.value();
  }
  else {
    error("unhandled item key type in DeletionLock: {}",
          std::to_underlying(ctx.key.key_type()));
    return HogDBError(HogDBCode::kInternal);
  }

  assert(ctx.dao);

  if (type == kRetrocausalQueue) {
    // Delegate to RCQ-specific cleanup: removes metadata + all blocks from DAO.
    // This is fully self-contained.
    auto cleanup_ret = RetrocausalQueueLock::cleanup_on_deletion(ctx);
    if (!cleanup_ret.has_value()) {
      return cleanup_ret.error();
    }
  }
  else {
    // For non-RCQ types, use the standard remove_item.
    HogDBStatus<void> remove_ret = ctx.dao->remove_item(ctx.key);
    if (!remove_ret) {
      return remove_ret;
    }
  }

  return {};
}

HogDBStatus<void>
DeletionLock::apply_close_hash_delta(const LockContext &ctx, LtHash16 &hash) const
{
  assert(ctx.dao);
  auto key = make_contract_data_db_key(ctx.key.as_bytes());
  auto before = ctx.dao->get_ramdb().get(key);
  if (before.has_value()) {
    lthash_remove_item(hash, key, before->type, before->value);
  }
  return {};
}

} // namespace hogd
