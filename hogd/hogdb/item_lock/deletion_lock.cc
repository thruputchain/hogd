#include "hogd/hogdb/item_lock/deletion_lock.h"

#include <utility>

#include "hogd/base/log.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/retrocausal_queue_lock.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<void>
DeletionLock::stage_committed_changes(const LockContext &ctx,
                                      ChangeSet &changes)
{
  if (ctx.key.is_address_auth_data()) {
    auto key =
        make_address_key(ctx.key.get_address(), Prefix::kAddressAuthData);
    changes.remove(key);
    return {};
  }

  if (!ctx.key.is_contract_data()) {
    error("unhandled item key type in DeletionLock: {}",
          std::to_underlying(ctx.key.key_type()));
    return HogDBError(HogDBCode::kInvalidExecutorBehavior);
  }

  CHECK(ctx.dao != nullptr);
  auto type_ret = ctx.dao->get_item_type(ctx.key);
  if (!type_ret) {
    if (type_ret.error() == HogDBCode::kNotFound) {
      return {};
    }
    return type_ret.error();
  }

  if (type_ret.value() == kRetrocausalQueue) {
    return RetrocausalQueueLock::cleanup_on_deletion(ctx, changes);
  }

  auto key = make_contract_data_key(ctx.key);
  changes.remove(key);
  return {};
}

} // namespace hogd
