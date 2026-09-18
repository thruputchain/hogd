#include "hogd/hog_db/item_lock/deletion_lock.h"

#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/change_set.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/item_lock/retrocausal_queue_lock.h"
#include "hogd/hog_db/status.h"

namespace hogd {

HogDbStatus<int128_t>
DeletionLock::try_atomic_change_lock(const Deletion * /*before*/,
                                     const Deletion *after,
                                     const LockContext &ctx)
{
  // `after == nullptr` is a lock release, not a new deletion (see
  // BlockTX::do_unlock_item, which CHECKs success); nothing to validate.
  if (after == nullptr) {
    return {0};
  }

  // Bound the block-close cleanup fan-out of a retrocausal-queue deletion (see
  // RetrocausalQueueLock::check_deletion_block_budget). This is the admission
  // path for a non-exclusive deletion; an exclusive deletion is instead
  // admitted through ExclusiveLock::try_atomic_change_lock, which applies the
  // same guard, so an over-budget delete fails as a transaction here rather
  // than at the commit-time cleanup.
  if (auto s = RetrocausalQueueLock::check_deletion_block_budget(ctx); !s) {
    return s.error();
  }
  return {0};
}

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
