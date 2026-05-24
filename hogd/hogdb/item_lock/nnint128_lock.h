#pragma once

#include <atomic>
#include <cassert>
#include <limits>
#include <utility>

#include "hogd/base/atomic.h"
#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace detail {

// Common functionality for NNInt128 and Balance locks.
class NNInt128DeltaBounds {
public:
  NNInt128DeltaBounds(int128_t base_value)
    : add_(add_capacity(base_value)), sub_(sub_capacity(base_value))
  {}

  // No-op if returns false.
  bool try_reserve_delta(int128_t delta_before, int128_t delta_after,
                         int128_t add_before, int128_t add_after,
                         int128_t sub_before, int128_t sub_after);

  int128_t get_delta() const { return delta_.load(std::memory_order_relaxed); }

  // For testing only
  uint128_t get_add_capacity() const { return add_.load(); }
  int128_t get_sub_capacity() const { return sub_.load(); }

private:
  static uint128_t add_capacity(int128_t base_value)
  {
    constexpr static uint128_t kInt128Max =
        std::numeric_limits<int128_t>::max();
    return kInt128Max - base_value;
  }

  static int128_t sub_capacity(int128_t base_value)
  {
    if (base_value <= 0) {
      return 0;
    }
    return base_value;
  }

  bool try_reserve_add_capacity(int128_t amount)
  {
    assert(amount >= 0);
    if (amount == 0) {
      return true;
    }
    uint128_t amount_uint = amount;

    uint128_t current_add = add_.load();

    for (;;) {
      if (current_add < amount_uint) {
        return false;
      }
      if (add_.compare_exchange_strong(current_add, current_add - amount_uint,
                                       std::memory_order_relaxed)) {
        return true;
      }
      cpu_relax();
    }
  }

  void release_add_capacity(int128_t amount)
  {
    assert(amount >= 0);
    if (amount == 0) {
      return;
    }
    add_.fetch_add(amount, std::memory_order_relaxed);
  }

  bool try_reserve_sub_capacity(int128_t amount)
  {
    assert(amount <= 0);
    if (amount == 0) {
      return true;
    }

    int128_t current_sub = sub_.load();

    for (;;) {
      assert(current_sub >= 0);
      // can't overflow
      if (current_sub + amount < 0) {
        return false;
      }
      if (sub_.compare_exchange_strong(current_sub, current_sub + amount,
                                       std::memory_order_relaxed)) {
        return true;
      }
      cpu_relax();
    }
  }

  void release_sub_capacity(int128_t amount)
  {
    assert(amount <= 0);
    if (amount == 0) {
      return;
    }
    sub_.fetch_sub(amount, std::memory_order_relaxed);
  }

  Atomic16B<int128_t> delta_ = 0;

  // nonnegative ints that represent
  // the amount that can be added to or subtracted from the base, without
  // overflowing. We subtract the (positive) add amount from add_, and add the
  // (negative) sub amount to sub_.
  Atomic16B<uint128_t> add_;
  Atomic16B<int128_t> sub_;
};

} // namespace detail

class NNInt128Lock {
public:
  NNInt128Lock(int128_t base_value)
    : base_(base_value), delta_bounds_(base_value)
  {}

  using change_type = NNInt128;

  // On success, returns a (deterministic) upper bound on the storage cost
  // required to commit the after value. Is a no-op on failure.
  // If after is nullptr, the storage cost output should be ignored.
  HogDBStatus<int128_t> try_atomic_change_lock(const NNInt128 *before,
                                               const NNInt128 *after,
                                               const LockContext &ctx);

  // Returns the amount of tokens that this commit is responsible for locking,
  // relative to the previously committed set of changes. Positive values
  // indicate tokens locked relative to prior to the transaction. The returned
  // value must be LTE the value returned by try_atomic_change_lock. The gap
  // (nonnegative) between the value returned by try_atomic_change_lock and the
  // value returned by this function is the amount of tokens that can be
  // released after the transaction commits.
  static int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                                NNInt128 & /*unused*/, const LockContext &ctx)
  {
    if (did_finalize) {
      return get_storage_cost_of_committed_value(ctx.cost_model) -
             snapshot_cost;
    }
    return 0;
  }

  static int128_t
  get_storage_cost_of_committed_value(const StorageCostModel *cost_model)
  {
    return cost_model->kBaseItemCost;
  }

  static bool is_committed_value_in_bounds(int128_t base, int128_t delta)
  {
    auto value = wrapping_add(base, delta);
    auto lower_bound = base < 0 ? base : int128_t{0};
    return value >= lower_bound;
  }

  HogDBStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

  // For balance items
  // Only should be called after all txs in a block are done.
  std::pair<int128_t, int128_t> get_base_and_delta() const
  {
    return {base_, delta_bounds_.get_delta()};
  }

  // For testing only
  uint128_t get_add_capacity() const
  {
    return delta_bounds_.get_add_capacity();
  }
  int128_t get_sub_capacity() const { return delta_bounds_.get_sub_capacity(); }

private:
  const int128_t base_;
  detail::NNInt128DeltaBounds delta_bounds_;
};

static_assert(sizeof(NNInt128Lock) == 64, "NNInt128Lock should be 64 bytes");
static_assert(item_lock<NNInt128Lock>);

} // namespace hogd
