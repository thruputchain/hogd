#include "hogd/hogdb/block_tx.h"

#include <cassert>
#include <mutex>
#include <utility>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<void>
ItemChange::commit(HogDatabase &db, ItemKey key) const
{
  if (std::holds_alternative<NNInt128>(change_)) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return db.write_nnint128(key, *to_nnint128());
  }
  if (std::holds_alternative<Bytes>(change_))
    return db.write_bytes(key, to_bytes());
  if (std::holds_alternative<Deletion>(change_))
    return db.remove(key);
  return {};
}

HogDBStatus<void>
ItemChange::apply(const ItemChange &change)
{
  // uninitialized changes can be replaced.
  if (std::holds_alternative<std::monostate>(change_)) {
    change_ = change.change_;
    return {};
  }

  // otherwise the existing type must be the same as the change type.
  if (change_.index() != change.change_.index())
    return HogDBError(HogDBCode::kConflict);

  // finally check for type-specific conflicts.

  // Handle NNInt128
  if (const NNInt128 *nval = std::get_if<NNInt128>(&change.change_); nval) {
    auto &oval = std::get<NNInt128>(change_);

    // any change to the base value is a conflict.
    if (oval.base != nval->base)
      return HogDBError(HogDBCode::kConflict);

    // check for overflow or negative sum.
    auto [new_delta, of1] = overflowing_add(oval.delta, nval->delta);
    if (of1)
      return HogDBError(HogDBCode::kOverflow);
    auto [new_base, of2] = overflowing_add(oval.base, new_delta);
    if (of2)
      return HogDBError(HogDBCode::kOverflow);
    if (new_base < 0)
      return HogDBError(HogDBCode::kConflict);

    // apply the change.
    oval.delta = new_delta;
    return {};
  }

  // Handle Bytes
  if (const auto *nval = std::get_if<Bytes>(&change.change_); nval) {
    const auto &oval = std::get<Bytes>(change_);
    if (oval.exclusive || nval->exclusive)
      return HogDBError(HogDBCode::kConflict);
    if (oval.buf != nval->buf)
      return HogDBError(HogDBCode::kConflict);
    return {};
  }

  // Handle Deletion
  assert(std::holds_alternative<Deletion>(change_));
  return {};
}

void
ItemChange::undo(const ItemChange &change)
{
  assert(change_.index() == change.change_.index());
  assert(!std::holds_alternative<std::monostate>(change_));

  const auto *nval = std::get_if<NNInt128>(&change.change_);
  if (nval == nullptr)
    return;

  // only NNInt128 requires undo so far.
  auto &oval = std::get<NNInt128>(change_);
  assert(oval.base == nval->base);
  oval.delta -= nval->delta;
}

HogDBStatus<void>
BlockTX::commit()
{
  std::mutex lock;
  HogDBStatus<void> ret;

  auto commit_fn = [this, &lock,
                    &ret](std::pair<const ItemKey, const ItemLock &> v) {
    HogDBStatus<void> tmp = v.second.change.commit(db_, v.first);
    if (!tmp) [[unlikely]] {
      std::scoped_lock guard(lock);
      ret = tmp;
    }
  };
  locks_.for_each_parallel(commit_fn);
  return ret;
}

} // namespace hogd
