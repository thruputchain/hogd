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

HogDBStatus<void>
InvokeTX::lock_storage(Address const &addr, int128_t cost)
{
  assert(cost >= 0);
  auto key = make_address_balance_item_key(addr);
  auto it = changes_.find(key);
  if (it == changes_.end() || it->second.change_vec.empty()) {
    auto bal_change = make_balance_change(block_.get_db(), addr, -cost);
    if (!bal_change)
      return bal_change.error();

    auto status = block_.lock(key, *bal_change);
    if (!status)
      return status.error();

    changes_[key].change_vec.emplace_back(std::move(*bal_change), tx_depth_);
    assert(changes_[key].locked_storage_cost == 0);
    return {};
  }

  auto last = it->second.change_vec.back();
  assert(last.change.is_nnint128());
  auto nnint = last.change.get_raw_nnint128();
  assert(nnint);

  auto new_nnint = NNInt128(nnint->base(), nnint->delta() - cost);
  ItemChange new_change(std::move(new_nnint));
  auto status = block_.modify_lock(key, last.change, new_change);
  if (!status)
    return status.error();

  if (last.tx_depth == tx_depth_) {
    last.change = std::move(new_change);
  }
  else {
    assert(last.tx_depth < tx_depth_);
    it->second.change_vec.emplace_back(std::move(new_change), tx_depth_);
  }

  assert(changes_[key].locked_storage_cost == 0);
  return {};
}

void
InvokeTX::commit()
{
  assert_is_valid();
  // changes are already applied to BlockTX, but we still need to mark them all
  // as committed
  for (auto &[key, vec] : changes_) {
    if (!vec.change_vec.empty()) {
      auto cost_change =
          block_.commit_change(key, vec.change_vec.back().change);
      assert(cost_change <= vec.locked_storage_cost);
      int128_t released_cost = vec.locked_storage_cost - cost_change;
      assert(released_cost >= 0);

      if (released_cost > 0) {
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

    if (!key.is_address_balance()) {
      if (!vec.change_vec.empty()) {
        block_.unlock(key, vec.change_vec.back().change);
      }
      assert(vec.locked_storage_cost >= 0);
      if (vec.locked_storage_cost > 0) {
        block_.release_balance(key.get_address(), vec.locked_storage_cost);
      }
    }
    else {
      assert(vec.locked_storage_cost == 0);
    }
  }
  changes_.clear();
}

void
InvokeTX::rollback_to_savepoint(int depth)
{
  assert_is_valid();

  tx_depth_ = depth;
  for (auto &[key, item_vec] : changes_) {
    auto &vec = item_vec.change_vec;
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
      assert(item_vec.locked_storage_cost >= *ret);
      continue;
    }
    block_.unlock(key, tmp);
  }
}

void
InvokeTX::release_savepoint(int depth)
{
  assert_is_valid();

  tx_depth_ = std::max(depth - 1, 0);
  for (auto &[key, vec] : changes_) {
    // skip if no changes made after savepoint.
    if (vec.change_vec.empty() || vec.change_vec.back().tx_depth <= tx_depth_)
      continue;

    // retain latest change but erase any history after savepoint.
    ItemVersion tmp{.change = std::move(vec.change_vec.back().change),
                    .tx_depth = tx_depth_};
    vec.change_vec.erase(std::ranges::upper_bound(vec.change_vec, tx_depth_, {},
                                                  &ItemVersion::tx_depth),
                         vec.change_vec.end());
    if (!vec.change_vec.empty() &&
        vec.change_vec.back().tx_depth == tx_depth_) {
      vec.change_vec.back().change = std::move(tmp.change);
      // TODO(geoff): abelay had left this as return; that seems incorrect.
      continue;
    }
    vec.change_vec.push_back(std::move(tmp));
  }
}

HogDBStatus<void>
InvokeTX::request_change(const ItemKey &key, ItemChange &&change)
{
  std::vector<ItemVersion> &v = changes_[key].change_vec;

  // this is the first change for this key; try to acquire a lock on the item.
  if (v.empty()) {
    HogDBStatus<int128_t> ret = block_.lock(key, change);
    if (!ret) {
      info("failed to lock storage for key {}", key);
      return ret.error();
    }
    v.emplace_back(std::move(change), tx_depth_);

    int128_t marginal_storage =
        std::max<int128_t>(0, *ret - changes_[key].locked_storage_cost);

    info("marginal storage for key {} = {}", key, *ret);
    // now lock the storage
    if (marginal_storage > 0) {
      if (auto r = lock_storage(key.get_address(), marginal_storage); !r) {
        info("failed to lock storage for key {}", key);
        return r.error();
      }
      changes_[key].locked_storage_cost += marginal_storage;
    }
    info("locked storage for key {}", key);
    return {};
  }

  // otherwise it is an existing change, so the item is already locked.
  ItemVersion &item = v.back();
  change.narrow(item.change);
  HogDBStatus<int128_t> ret = block_.modify_lock(key, item.change, change);
  if (!ret) {
    info("failed to modify storage lock for key {}", key);
    return ret.error();
  }
  if (item.tx_depth == tx_depth_) {
    item.change = std::move(change);
  }
  else {
    assert(item.tx_depth < tx_depth_);
    v.emplace_back(std::move(change), tx_depth_);
  }
  // now to lock the storage
  int128_t marginal_storage =
      std::max<int128_t>(0, *ret - changes_[key].locked_storage_cost);
  if (marginal_storage > 0) {
    if (auto r = lock_storage(key.get_address(), marginal_storage); !r) {
      return r.error();
    }
    changes_[key].locked_storage_cost += marginal_storage;
  }
  info("modified storage lock for key {}", key);
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
