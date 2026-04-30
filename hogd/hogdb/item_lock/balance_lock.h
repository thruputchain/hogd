#pragma once

#include <limits>
#include <optional>

#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace detail {
class LazyBalance {
public:
  LazyBalance(std::optional<int128_t> snapshot_value)
    : snapshot_value_(snapshot_value.has_value()
                          ? *snapshot_value
                          : std::numeric_limits<int128_t>::min())
  {}

  std::optional<int128_t> snapshot_value() const
  {
    return snapshot_value_ >= 0 ? std::make_optional<int128_t>(snapshot_value_)
                                : std::nullopt;
  }

private:
  // Balances, unlike nnint128, are *always* nonnegative.
  // There is no set() available on a balance.
  // Thus, we can save 8 bytes (over std::optional<int128_t>) for a lazily
  // loaded balance via the following representation. Negative value here
  // implies the balance does not exist in the snapshot. nonnegative value
  // implies the balance exists in the snapshot.
  int128_t snapshot_value_;
};

} // namespace detail

// A lock that can be used to lock a balance item.
class BalanceLock {
public:
  using change_type = Balance;

  BalanceLock(std::optional<int128_t> snapshot_value)
    : snapshot_balance_(snapshot_value),
      delta_bounds_(snapshot_value.has_value() ? *snapshot_value : 0)
  {}

  HogDBStatus<int128_t> try_atomic_change_lock(const Balance *before,
                                               const Balance *after,
                                               const LockContext &ctx);

  // commit_change, for other item lock types, returns the storage cost that can
  // be released after committing this change. Since this is a balance, we just
  // do that accounting internally to the balance lock.
  // We *could* use this to relax the condition that if snapshot_value_ is
  // nullopt then you need to supply the full storage cost in one tx. That would
  // require differentiating block validation from block production, which we do
  // not currently do.
  static int128_t commit_change(bool /* finalize */,
                                int128_t /* snapshot_cost */,
                                const Balance & /* after */,
                                const LockContext & /*ctx*/)
  {
    return 0;
  }

  void add_committed_credit(int128_t delta);

  HogDBStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

  static int128_t
  get_storage_cost_of_committed_value(const StorageCostModel *cost_model)
  {
    return cost_model->get_balance_cost();
  }

  int128_t build_result(LockContext const &ctx) const;

  int128_t get_delta() const { return delta_bounds_.get_delta(); }

  // For testing only
  int128_t get_add_capacity() const { return delta_bounds_.get_add_capacity(); }
  int128_t get_sub_capacity() const { return delta_bounds_.get_sub_capacity(); }

private:
  detail::LazyBalance snapshot_balance_;
  detail::NNInt128DeltaBounds delta_bounds_;
};

} // namespace hogd
