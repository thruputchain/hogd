#include "hogd/hog_db/item_lock/exclusive_lock.h"

#include <cassert>
#include <set>

#include "db_conflict.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/item_lock/retrocausal_queue_lock.h"
#include "hogd/hog_db/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "x23/debug.h"

namespace hogd {

// NOLINTBEGIN(readability-convert-member-functions-to-static)
HogDbStatus<int128_t>
ExclusiveLock::try_atomic_change_lock(const ItemChange * /*unused*/,
                                      const ItemChange *after,
                                      const LockContext &ctx)
{
  assert(after != nullptr);
  assert(ctx.cost_model != nullptr);

  // TODO(geoff): This should be contained inside ItemChange;
  // but I do not want to mess storage accounting inside ItemChange yet

  if (after->is_bytes()) {
    // Read the size in place rather than via to_bytes(), which would copy
    // the whole bytestring just to measure it.
    auto const *bytes = after->get_inner<Bytes>();
    CHECK_PTR(bytes);
    return ctx.cost_model->get_bytestring_cost(bytes->value_view().size());
  }
  if (after->is_nnint128()) {
    return ctx.cost_model->get_nnint_cost();
  }
  if (after->is_deletion()) {
    return 0;
  }
  if (after->is_deploy_contract()) {
    // Read the change in place rather than via to_new_deployment(), which
    // would copy the whole DeployedContractData into an optional just to
    // read one field out of it.
    auto const *deployment = after->get_inner<DeployedContractData>();
    CHECK_PTR(deployment);
    return ctx.cost_model->get_deployment_cost(
        deployment->deployed_contract_hash());
  }
  if (after->is_auth_data()) {
    // Read the change in place rather than via to_auth_data(), which would
    // copy the whole auth table into an optional.
    auto const *auth_data = after->get_inner<PointerItem<AuthDataItem>>();
    CHECK_PTR(auth_data);
    auto const &item = **auth_data;
    // Refused here, at reservation: the AuthLock admission that commit_change
    // runs comes after the transaction's balance changes are committed, and
    // a conflict there poisons the block.
    if (item.introduces_duplicate_signer()) {
      return ctx.conflict(DbConflictCode::DuplicateSigner);
    }
    // A change that leaves no row reserves nothing, as AuthLock charges it.
    if (!item.writes_row()) {
      return 0;
    }
    return ctx.cost_model->get_auth_data_cost(item.count_active_keys());
  }
  if (after->is_retrocausal_queue()) {
    // Delegate validation and consume-counting to RetrocausalQueueLock.
    // We intentionally discard its returned cost and compute our own tight
    // cost that accounts for consumes immediately, since there is no
    // concurrency concern under an exclusive lock.
    // Read the change in place rather than via to_retrocausal_queue(), which
    // would deep-copy the entire typed cumulative delta (nested QueueEntry
    // buffers and payloads) into an optional just to validate it. Mirrors the
    // is_deploy_contract() branch above.
    auto const *inner = after->get_inner<RetrocausalQueueIt>();
    CHECK_PTR(inner);

    RetrocausalQueueLock lock;
    auto cost = lock.try_atomic_change_lock(nullptr, inner, ctx);
    if (!cost) {
      return cost;
    }

    // Under exclusive lock there is no concurrency, so every
    // current-block consumed entry must have been added by this same TX.
    // Verify identity: each consumed entry exists in added_entries.
    if (inner->delta().consume &&
        !inner->delta().consume->current_block_consumed.empty()) {
      auto const &consumed = inner->delta().consume->current_block_consumed;
      auto const &added = inner->delta().insert.added_entries;

      std::multiset<QueueEntry> added_set(added.begin(), added.end());
      for (auto const &entry : consumed) {
        auto it = added_set.find(entry);
        if (it == added_set.end()) {
          return HogDbError{HogDbCode::kInvalidExecutorBehavior};
        }
        added_set.erase(it);
      }
    }

    // try_atomic_change_lock validated everything and stashed
    // consumed_past_entries_ on the delta.  get_meta returns the
    // already-cached metadata (no extra DAO read).
    auto meta = lock.get_meta(nullptr);
    CHECK(meta.has_value());

    uint64_t live = (*meta)->live_entries;
    uint64_t added = inner->delta().insert.added_entries.size();
    uint64_t consumed = inner->consumed_past_entries_;
    if (inner->delta().consume) {
      consumed += inner->delta().consume->current_block_consumed.size();
    }

    if (consumed > live + added) {
      return HogDbError{HogDbCode::kInvalidExecutorBehavior};
    }
    uint64_t total = live + added - consumed;
    if (total == 0) {
      return int128_t{0};
    }
    return ctx.cost_model->get_retrocausal_queue_cost(total);
  }
  panic("unimplemented");
}
// NOLINTEND(readability-convert-member-functions-to-static)

} // namespace hogd
