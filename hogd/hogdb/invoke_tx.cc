#include "hogd/hogdb/invoke_tx.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<int128_t>
InvokeTX::read_nnint128(const ItemKey &key)
{
  const ItemChange *change = read_change(key);
  if (!change)
    return hogd::read_nnint128(block_.get_db(), key);
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
    return hogd::read_bytes(block_.get_db(), key);
  if (change->is_deletion())
    return HogDBError(HogDBCode::kNotFound);
  OwningSpan<const std::byte> ret = change->to_bytes();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return ret;
}

void
InvokeTX::commit()
{
  assert_is_valid();

  // changes are already applied to BlockTX, so we merely clear them here.
  changes_.clear();
}

void
InvokeTX::abort()
{
  assert_is_valid();

  for (auto &[key, vec] : changes_) {
    if (!vec.empty())
      block_.unlock(key, vec.back().change);
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
      [[maybe_unused]] HogDBStatus<void> ret =
          block_.modify_lock(key, tmp, vec.back().change);
      assert(ret);
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
      return;
    }
    vec.push_back(std::move(tmp));
  }
}

HogDBStatus<void>
InvokeTX::request_change(const ItemKey &key, ItemChange &&change)
{
  std::vector<ItemVersion> &v = changes_[key];

  // this is the first change for this key; try to acquire a lock on the item.
  if (v.empty()) {
    std::println("request_change: lock key: {}", key);
    HogDBStatus<void> ret = block_.lock(key, change);
    if (!ret)
      return ret.error();
    std::println("request_change: lock success key: {}", key);
    v.emplace_back(std::move(change), tx_depth_);
    return {};
  }

  // otherwise it is an existing change, so the item is already locked.
  ItemVersion &item = v.back();
  change.narrow(item.change);
  HogDBStatus<void> ret = block_.modify_lock(key, item.change, change);
  if (!ret)
    return ret.error();
  if (item.tx_depth == tx_depth_) {
    item.change = std::move(change);
  }
  else {
    assert(item.tx_depth < tx_depth_);
    v.emplace_back(std::move(change), tx_depth_);
  }
  return {};
}

} // namespace hogd
