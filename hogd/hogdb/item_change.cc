#include "hogd/hogdb/item_change.h"

#include <cassert>
#include <concepts>
#include <span>
#include <type_traits>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/status.h"

namespace hogd {

inline HogDBStatus<void>
NNInt128::commit(HogDatabase &db, ItemKey key) const
{
  return db.write_nnint128(key, value());
}

inline HogDBStatus<void>
NNInt128::merge(const NNInt128 &nnint)
{
  assert_is_valid();
  nnint.assert_is_valid();

  // base must be the same to merge.
  if (base_ != nnint.base_)
    return HogDBError(HogDBCode::kConflict);

  // calculate new bounds.
  auto [plus, of1] = overflowing_add(plus_, nnint.plus_);
  auto [minus, of2] = overflowing_add(minus_, nnint.minus_);
  if (of1 || of2)
    return HogDBError(HogDBCode::kOverflow);

  // check if could go negative.
  if (base_ + minus < 0)
    return HogDBError(HogDBCode::kConflict);

  // check if could overflow.
  auto [ign, of3] = overflowing_add(base_, plus);
  if (of3)
    return HogDBError(HogDBCode::kOverflow);

  // all checks passed; perform the merge.
  plus_ = plus;
  minus_ = minus;
  delta_ += nnint.delta_;

  assert_is_valid();
  return {};
}

inline void
NNInt128::undo_merge(const NNInt128 &nnint)
{
  assert_is_valid();
  nnint.assert_is_valid();

  plus_ -= nnint.plus_;
  minus_ -= nnint.minus_;
  delta_ -= nnint.delta_;

  assert_is_valid();
}

inline HogDBStatus<void>
Bytes::commit(HogDatabase &db, ItemKey key) const
{
  return db.write_bytes(key, std::as_bytes(std::span(buf_)));
}

inline HogDBStatus<void>
Deletion::commit(HogDatabase &db, ItemKey key)
{
  return db.remove(key);
}

namespace {

template<typename T>
concept is_monostate = std::same_as<std::decay_t<T>, std::monostate>;

} // namespace

HogDBStatus<void>
ItemChange::commit(HogDatabase &db, ItemKey key) const
{
  return std::visit(
      [&db, &key](auto &arg) -> HogDBStatus<void> {
        if constexpr (!is_monostate<decltype(arg)>)
          return arg.commit(db, key);
        return {};
      },
      type_);
}

HogDBStatus<void>
ItemChange::merge(const ItemChange &change)
{
  if (type_.index() != change.type_.index())
    return HogDBError(HogDBCode::kConflict);

  return std::visit(
      [&change](auto &arg) -> HogDBStatus<void> {
        if constexpr (!is_monostate<decltype(arg)>)
          return arg.merge(std::get<std::decay_t<decltype(arg)>>(change.type_));
        return {};
      },
      type_);
}

void
ItemChange::undo_merge(const ItemChange &change)
{
  assert(type_.index() == change.type_.index());

  std::visit(
      [&change](auto &arg) {
        if constexpr (!is_monostate<decltype(arg)>)
          arg.undo_merge(std::get<std::decay_t<decltype(arg)>>(change.type_));
      },
      type_);
}

size_t
ItemChange::storage_size() const
{
  return std::visit(
      [](auto &arg) -> size_t {
        if constexpr (!is_monostate<decltype(arg)>)
          return arg.storage_size();
        return 0;
      },
      type_);
}

} // namespace hogd
