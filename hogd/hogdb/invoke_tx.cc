#include "hogd/hogdb/invoke_tx.h"

// TODO(amb): remove when llvm adds std::ranges::upper_bound to include cleaner.
#include <algorithm> // NOLINT(misc-include-cleaner)
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<int128_t>
InvokeTX::read_nnint128(ItemKey key)
{
  const ItemChange *change = read_change(key);
  if (!change)
    return block_.read_nnint128(key);
  std::optional ret = change->to_nnint128();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return *ret;
}

HogDBStatus<OwningSpan<const std::byte>>
InvokeTX::read_bytes(ItemKey key)
{
  const ItemChange *change = read_change(key);
  if (!change)
    return block_.read_bytes(key);
  OwningSpan<const std::byte> ret = change->to_bytes();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return ret;
}

void
InvokeTX::commit()
{
  // changes are already applied to BlockTX, so we merely clear them here.
  changes_.clear();
}

void
InvokeTX::abort()
{
  for (const auto &[key, vec] : changes_) {
    if (!vec.empty())
      block_.unlock(key, vec.back().change);
  }
  changes_.clear();
}

HogDBStatus<void>
InvokeTX::rollback_to_savepoint(int depth)
{
  tx_depth_ = depth - 1;
  for (auto &[key, vec] : changes_) {
    // skip if no changes made after savepoint.
    if (vec.empty() || vec.back().tx_depth <= tx_depth_)
      continue;

    // revert and erase the history of changes made after savepoint.
    ItemChange tmp{std::move(vec.back().change)};
    // NOLINTNEXTLINE(misc-include-cleaner)
    vec.erase(std::ranges::upper_bound(vec, tx_depth_, {},
                                       &ItemVersion::tx_depth),
              vec.end());
    if (!vec.empty()) {
      HogDBStatus<void> ret = block_.modify_lock(key, tmp, vec.back().change);
      if (!ret)
        return ret.error();
      continue;
    }
    block_.unlock(key, tmp);
  }

  return {};
}

void
InvokeTX::release_savepoint(int depth)
{
  tx_depth_ = depth - 1;
  for (auto &[key, vec] : changes_) {
    // skip if no changes made after savepoint.
    if (vec.empty() || vec.back().tx_depth <= tx_depth_)
      continue;

    // retain latest change but erase any history after savepoint.
    ItemVersion tmp{.change = std::move(vec.back().change),
                    .tx_depth = tx_depth_};
    // NOLINTNEXTLINE(misc-include-cleaner)
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
InvokeTX::request_change(ItemKey key, ItemChange &&change)
{
  std::vector<ItemVersion> &v = changes_[key];

  // this is the first change for this key; try to acquire a lock on the item.
  if (v.empty()) {
    HogDBStatus<void> ret = block_.lock(key, change);
    if (!ret)
      return ret.error();
    v.emplace_back(std::move(change), tx_depth_);
    return {};
  }

  // otherwise it is an existing change, so the item is already locked.
  ItemVersion &item = v.back();
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
