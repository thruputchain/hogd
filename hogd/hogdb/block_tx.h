// block_tx.h - transaction handling logic at the granularity of a block.

#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/status.h"

namespace hogd {

//
// These track changes for the item types currently supported by hogdb:
//
// NNInt128: a non-negative integer type that allows commutative operations
// without conflicts.
//
// Bytes: A variable-length vector of bytes (also called a byte string). Used
// to store arbitrary opaque data. If exclusive, any changes to the same key
// will conflict. Otherwise, will commute if the changes are the same value.
//
// Deletion: Used as a placeholder to indicate the item should be removed.
//

struct NNInt128 {
  int128_t base;
  int128_t delta;
};

struct Bytes {
  std::vector<std::byte> buf;
  bool exclusive;
};

struct Deletion {};

// ItemChange represents a pending modification to an item.
class ItemChange {
public:
  ItemChange() = default;
  ItemChange(NNInt128 &&nnint) : change_(std::move(nnint)) {}
  ItemChange(Bytes &&bytes) : change_(std::move(bytes)) {}
  ItemChange(Deletion &&d) : change_(std::move(d)) {}
  ~ItemChange() = default;

  // getters
  std::optional<int128_t> to_nnint128() const;
  OwningSpan<const std::byte> to_bytes() const;
  bool is_deletion() const { return std::holds_alternative<Deletion>(change_); }

  // commit applies the item's changes to the ramdb.
  HogDBStatus<void> commit(HogDatabase &db, ItemKey key) const;

  // apply merges a change into this one. It can return kConflict if the change
  // does not commute.
  HogDBStatus<void> apply(const ItemChange &change);
  // undo reverts a change from this one.
  void undo(const ItemChange &change);

private:
  std::variant<std::monostate, NNInt128, Bytes, Deletion> change_;
};

inline std::optional<int128_t>
ItemChange::to_nnint128() const
{
  const auto *tmp = std::get_if<NNInt128>(&change_);
  if (!tmp)
    return std::nullopt;
  return tmp->base + tmp->delta;
}

inline OwningSpan<const std::byte>
ItemChange::to_bytes() const
{
  const auto *tmp = std::get_if<Bytes>(&change_);
  if (!tmp)
    return {};
  std::vector<std::byte> buf(tmp->buf);
  auto span = std::span<const std::byte>(buf);
  return OwningSpan(span, [ref = std::move(buf)] {});
}

// BlockTX manages a set of pending changes made in a block relative to the
// current snapshot state. They can be applied to the ramdb via commit().
// Multiple BlockTXes can be instantiated, but only one should be committed.
class BlockTX {
public:
  BlockTX(HogDatabase &db) : db_(db) {}
  ~BlockTX() = default;

  //
  // The read_* methods below read the current state of an item in the block
  // view. They will return kTypeMismatch if the stored type is different from
  // the requested type or kNotFound if the item does not exist.
  //

  HogDBStatus<int128_t> read_nnint128(ItemKey key);
  HogDBStatus<OwningSpan<const std::byte>> read_bytes(ItemKey key);

  // lock reserves the right to change an item.
  HogDBStatus<void> lock(ItemKey key, const ItemChange &change);
  // replace modifies an existing request to change an item.
  HogDBStatus<void> replace(ItemKey key, const ItemChange &old_change,
                            const ItemChange &new_change);
  // unlock drops and undoes a request to change an item.
  void unlock(ItemKey key, const ItemChange &change);

  // commit applies all changes to the database.
  HogDBStatus<void> commit();

private:
  struct ItemLock {
    ItemChange change;
    size_t ref;
  };

  HogDatabase &db_;
  SynchronizedMap<ItemKey, ItemLock> locks_;
};

inline HogDBStatus<int128_t>
BlockTX::read_nnint128(ItemKey key)
{
  auto [val, ul] = locks_.find(key);
  if (!val)
    return db_.read_nnint128(key);
  std::optional ret = val->change.to_nnint128();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return *ret;
}

inline HogDBStatus<OwningSpan<const std::byte>>
BlockTX::read_bytes(ItemKey key)
{
  auto [val, ul] = locks_.find(key);
  if (!val)
    return db_.read_bytes(key);
  OwningSpan<const std::byte> ret = val->change.to_bytes();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return ret;
}

inline HogDBStatus<void>
BlockTX::lock(ItemKey key, const ItemChange &change)
{
  auto [val, ul] = locks_[key];
  val.ref++;
  return val.change.apply(change);
}

inline HogDBStatus<void>
BlockTX::replace(ItemKey key, const ItemChange &old_change,
                 const ItemChange &new_change)
{
  auto [val, ul] = locks_[key];
  if (val.ref == 1) {
    val.change = new_change;
    return {};
  }

  ItemChange tmp = val.change;
  tmp.undo(old_change);
  HogDBStatus<void> ret = tmp.apply(new_change);
  if (!ret)
    return ret.error();

  val.change = tmp;
  return {};
}

inline void
BlockTX::unlock(ItemKey key, const ItemChange &change)
{
  auto [val, ul] = locks_[key];
  if (--val.ref == 0) {
    val.change = ItemChange();
    return;
  }
  val.change.undo(change);
}

} // namespace hogd
