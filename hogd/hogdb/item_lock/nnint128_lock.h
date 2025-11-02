#pragma once

#include <atomic>
#include <cassert>
#include <limits>

#include "hogd/base/atomic.h"
#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class NNInt128Lock {
public:
  NNInt128Lock(int128_t snapshot_value)
    : base_(snapshot_value), add_(add_capacity(snapshot_value)),
      sub_(sub_capacity(snapshot_value))
  {}

  // On success, returns a (deterministic) upper bound on the storage cost
  // required to commit the after value. Is a no-op on failure.
  HogDBStatus<int128_t>
  try_atomic_change_lock(const NNInt128 *before, const NNInt128 *after,
                         const StorageCostModel *cost_model);

  static int128_t
  get_storage_cost_of_committed_value(const StorageCostModel *cost_model)
  {
    return cost_model->kBaseItemCost;
  }

  constexpr static int128_t add_capacity(int128_t snapshot_value)
  {
    if (snapshot_value >= 0) {
      return std::numeric_limits<int128_t>::max() - snapshot_value;
    }
    return std::numeric_limits<int128_t>::max();
  }

  constexpr static int128_t sub_capacity(int128_t snapshot_value)
  {
    if (snapshot_value <= 0) {
      return 0;
    }
    return snapshot_value;
  }

  // For testing only
  int128_t get_add_capacity() const { return add_.load(); }

  int128_t get_sub_capacity() const { return sub_.load(); }

private:
  const int128_t base_;
  Atomic16B<int128_t> delta_ = 0;
  // nonnegative ints that represent
  // the amount that can be added to or subtracted from the base, without
  // overflowing. We subtract the (positive) add amount from add_, and add the
  // (negative) sub amount to sub_.
  Atomic16B<int128_t> add_;
  Atomic16B<int128_t> sub_;

  bool try_reserve_add_capacity(int128_t amount)
  {
    assert(amount >= 0);

    int128_t current_add = add_.load();

    for (;;) {
      if (current_add < amount) {
        return false;
      }
      if (add_.compare_exchange_strong(current_add, current_add - amount,
                                       std::memory_order_relaxed)) {
        return true;
      }
      cpu_relax();
    }
  }

  void release_add_capacity(int128_t amount)
  {
    assert(amount >= 0);
    add_.fetch_add(amount, std::memory_order_relaxed);
  }

  bool try_reserve_sub_capacity(int128_t amount)
  {
    assert(amount <= 0);

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
    sub_.fetch_sub(amount, std::memory_order_relaxed);
  }
};

static_assert(sizeof(NNInt128Lock) == 64, "NNInt128Lock should be 64 bytes");

} // namespace hogd
