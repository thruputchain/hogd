#include "hogd/hog_db/item_lock/deletion_lock.h"

#include <utility>

#include "hogd/base/log.h"
#include "hogd/hog_db/change_set.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/item_lock/retrocausal_queue_lock.h"
#include "hogd/hog_db/status.h"

namespace hogd {

HogDbStatus<void>
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
    return HogDbError(HogDbCode::kInvalidExecutorBehavior);
  }

  CHECK(ctx.dao != nullptr);
  auto type_ret = ctx.dao->get_item_type(ctx.key);
  if (!type_ret) {
    if (type_ret.error() == HogDbCode::kNotFound) {
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
