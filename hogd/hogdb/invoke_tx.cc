#include "hogd/hogdb/invoke_tx.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<int128_t>
InvokeTX::read_nnint128(const ItemKey &key)
{
  const ItemChange *change = read_change(key);
  if (!change)
    return block_.get_db().read_nnint128(key);
  if (change->is_deletion())
    return HogDBError(HogDBCode::kNotFound);
  std::optional ret = change->to_nnint128();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return *ret;
}

HogDBStatus<OwningSpan<const std::byte>>
InvokeTX::read_bytes(const ItemKey &key)
{
  const ItemChange *change = read_change(key);
  if (!change)
    return block_.get_db().read_bytes(key);
  if (change->is_deletion())
    return HogDBError(HogDBCode::kNotFound);
  OwningSpan<const std::byte> ret = change->to_bytes();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return ret;
}

// Is only responsible for updating the lock on the balance item;
// not for updating anything else
HogDBStatus<void>
InvokeTX::update_balance_lock(Address const& addr)
{
  int128_t storage_adjustment = 0;
  auto bal = balance_deltas_[addr];
  if (!bal.empty()) {
    auto [ign, of] = overflowing_add(bal.back().total_locked_storage_amount, bal.back().manual_transfer_amount);
    if (of) {
      return HogDBError(HogDBCode::kOverflow);
    }
    storage_adjustment = ign;
  }

  auto key = make_address_balance_item_key(addr);
  auto change = changes_[key];
  if (change.empty()) {
    auto base = block_.get_db().read_address_balance(addr);
    if (!base) {
      return base.error();
    }
    auto nnint = NNInt128(*base, storage_adjustment);
    auto [ign, of] = overflowing_add(nnint.base(), nnint.delta());
    if (of) {
      return HogDBError(HogDBCode::kOverflow);
    }
    if (ign < 0) {
      return HogDBError(HogDBCode::kConflict);
    }
    auto new_item = ItemChange(std::move(nnint));

    auto status = block_.lock(key, new_item);
    if (!status) {
      return status.error();
    }
    change.emplace_back(std::move(new_item), tx_depth_, 0);
    return {};
  }

  auto last = change.back();
  assert(last.tx_depth <= tx_depth_);
  assert(last.change.is_nnint128());
  auto nnint = last.change.get_raw_nnint128();
  assert(nnint);





}

HogDBStatus<void>
InvokeTX::lock_storage(Address const &addr, int128_t cost)
{
  info("lock_storage: {} {}", addr, cost);
  auto key = make_address_balance_item_key(addr);
  auto it = changes_.find(key);
  if (it == changes_.end() || it->second.empty()) {
    auto bal_change = make_balance_change(block_.get_db(), addr, -cost);
    if (!bal_change)
      return bal_change.error();

    auto nnint = bal_change->get_raw_nnint128();
    assert(nnint);
    // make sure that the proposed change is valid in itself.
    // TODO(geoff): If we switch to retrocausal nnint, this check could be
    // relaxed.
    auto [ign, of] = overflowing_add(nnint->base(), nnint->delta());
    if (of) {
      return HogDBError(HogDBCode::kOverflow);
    }
    if (ign < 0) {
      return HogDBError(HogDBCode::kConflict);
    }

    info("doing new lock storage: base {} amount {}", nnint->base(),  nnint -> delta());
    auto status = block_.lock(key, *bal_change);
    if (!status)
      return status.error();

    changes_[key].emplace_back(std::move(*bal_change), tx_depth_, 0);
    assert(changes_[key].back().locked_storage_cost == 0);
    return {};
  }

  auto last = it->second.back();
  assert(last.change.is_nnint128());
  auto nnint = last.change.get_raw_nnint128();
  assert(nnint);

  auto [new_delta, of] = overflowing_add(nnint->delta(), -cost);
  if (of) {
    return HogDBError(HogDBCode::kOverflow);
  }

  // TODO(geoff): If we switch to retrocausal nnint, this check could be relaxed
  // That would require relaxing the nnint assert_is_valid check below
  auto [ign, of2] = overflowing_add(nnint->base(), new_delta);
  if (of2) {
    return HogDBError(HogDBCode::kOverflow);
  }
  if (ign < 0) {
    return HogDBError(HogDBCode::kConflict);
  }

  auto new_nnint = NNInt128(nnint->base(), new_delta);
  ItemChange new_change(std::move(new_nnint));
  new_change.narrow(last.change);
  auto status = block_.modify_lock(key, last.change, new_change);
  if (!status)
    return status.error();

  if (last.tx_depth == tx_depth_) {
    last.change = std::move(new_change);
  }
  else {
    assert(last.tx_depth < tx_depth_);
    it->second.emplace_back(std::move(new_change), tx_depth_, 0);
  }

  // balance items don't lock storage
  assert(changes_[key].back().locked_storage_cost == 0);
  return {};
}

void
InvokeTX::commit()
{
  assert_is_valid();
  // changes are already applied to BlockTX, but we still need to mark them all
  // as committed
  for (auto &[key, vec] : changes_) {
    if (!vec.empty()) {
      auto cost_change = block_.commit_change(key, vec.back().change);
      assert(cost_change <= vec.back().locked_storage_cost);
      // NONDETERMINISTIC:Compute the amount of storage tokens that can be
      // released after this transaction commits.
      int128_t released_cost = vec.back().locked_storage_cost - cost_change;
      assert(released_cost >= 0);

      if (released_cost > 0) {
        info("release_balance: {} {}", key.get_address(), released_cost);
        block_.release_balance(key.get_address(), released_cost);
      }
    }
  }
  changes_.clear();
}

void
InvokeTX::abort()
{
  assert_is_valid();

  for (auto &[key, vec] : changes_) {
    if (!vec.empty()) {
      block_.unlock(key, vec.back().change);
    }
  }
  changes_.clear();
}

void
InvokeTX::rollback_to_savepoint(int depth)
{
  assert_is_valid();

  tx_depth_ = depth;
  for (auto &[key, vec] : changes_) {
    // skip if no changes made after savepoint.
    if (vec.empty() || vec.back().tx_depth <= tx_depth_)
      continue;

    // revert and erase the history of changes made after savepoint.
    ItemChange tmp{std::move(vec.back().change)};
    vec.erase(std::ranges::upper_bound(vec, tx_depth_, {},
                                       &ItemVersion::tx_depth),
              vec.end());
    if (!vec.empty()) {
      // can't fail because reservation <= than previous one.
      [[maybe_unused]] HogDBStatus<int128_t> ret =
          block_.modify_lock(key, tmp, vec.back().change);
      assert(ret);
      assert(vec.back().locked_storage_cost >= *ret);
      continue;
    }
    block_.unlock(key, tmp);
  }

  for (auto &[addr, vec] : balance_deltas_) {
    vec.erase(std::ranges::upper_bound(vec, tx_depth_, {},
                                       &StorageBalanceVersion::tx_depth),
              vec.end());
  }
}

void
InvokeTX::release_savepoint(int depth)
{
  assert_is_valid();

  tx_depth_ = std::max(depth - 1, 0);
  for (auto &[key, vec] : changes_) {
    // skip if no changes made after savepoint.
    if (vec.empty() || vec.back().tx_depth <= tx_depth_)
      continue;

    // retain latest change but erase any history after savepoint.
    ItemVersion tmp{.change = std::move(vec.back().change),
                    .tx_depth = tx_depth_};
    vec.erase(std::ranges::upper_bound(vec, tx_depth_, {},
                                       &ItemVersion::tx_depth),
              vec.end());
    if (!vec.empty() && vec.back().tx_depth == tx_depth_) {
      vec.back().change = std::move(tmp.change);
      // TODO(geoff): abelay had left this as return; that seems incorrect.
      continue;
    }
    vec.push_back(std::move(tmp));
  }

  throw std::runtime_error("not implemented releasing balance versions");
}

HogDBStatus<void>
InvokeTX::request_change(const ItemKey &key, ItemChange &&change)
{
  std::vector<ItemVersion> &v = changes_[key];

  // this is the first change for this key; try to acquire a lock on the item.
  if (v.empty()) {
    HogDBStatus<int128_t> ret = block_.lock(key, change);
    if (!ret) {
      return ret.error();
    }
    v.emplace_back(std::move(change), tx_depth_);

    int128_t marginal_storage = std::max<int128_t>(0, *ret);

    // now lock the storage
    if (marginal_storage > 0) {
      if (auto r = lock_storage(key.get_address(), marginal_storage); !r) {
        return r.error();
      }
      v.back().locked_storage_cost = marginal_storage;
    }
    assert_is_valid();
    return {};
  }

  // otherwise it is an existing change, so the item is already locked.
  ItemVersion &item = v.back();
  change.narrow(item.change);
  HogDBStatus<int128_t> ret = block_.modify_lock(key, item.change, change);
  if (!ret) {
    return ret.error();
  }
  int128_t previous_locked_storage = item.locked_storage_cost;

  if (item.tx_depth == tx_depth_) {
    item.change = std::move(change);
  }
  else {
    assert(item.tx_depth < tx_depth_);
    v.emplace_back(std::move(change), tx_depth_, previous_locked_storage);
  }
  // now to lock the storage
  int128_t marginal_storage =
      std::max<int128_t>(0, *ret - previous_locked_storage);
  if (marginal_storage > 0) {
    if (auto r = lock_storage(key.get_address(), marginal_storage); !r) {
      return r.error();
    }
    v.back().locked_storage_cost += marginal_storage;
  }
  assert_is_valid();
  return {};
}

HogDBStatus<void>
InvokeTX::request_nnint128(const ItemKey &key, int128_t base, int128_t delta)
{
  // validate the nnint128. We can't assume it's in a valid state even if there
  // is not a conflict with another change because the executor may not know
  // the current base value.
  auto [sum, of] = overflowing_add(base, delta);
  if (of) [[unlikely]] {
    return HogDBError(HogDBCode::kOverflow);
  }
  if (sum < 0) {
    return HogDBError(HogDBCode::kConflict);
  }

  // nnint128 is valid; now try to request the change.
  return request_change(key, NNInt128{base, delta});
}

HogDBStatus<void>
InvokeTX::request_nnint128(const ItemKey &key, int128_t delta)
{
  HogDBStatus<int128_t> base = block_.get_db().read_nnint128(key);
  if (!base) {
    if (base.error() != HogDBCode::kNotFound)
      return base.error();
    base = 0;
  }
  return request_nnint128(key, *base, delta);
}

} // namespace hogd
