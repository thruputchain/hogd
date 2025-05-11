// item_change.h - modification tracking for DB items.
//
// Design Overview:
//
// Each ItemChange tracks modifications to an item.  ItemChange's can be merged
// with merge(), allowing for multiple changes to the same item as long as they
// are the same type and commute (i.e., no conflicts). Each type of ItemChange
// has different rules for what is considered a conflict.
//

#pragma once

#include <cassert>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/status.h"

namespace hogd {

// is_item is a concept that defines the required methods for a DB item.
template<typename T>
concept is_item = requires(T t, HogDatabase &db, ItemKey key, const T &arg) {
  // applies the change to the ramdb.
  { t.commit(db, key) } -> std::same_as<HogDBStatus<void>>;
  // merges together the change with arg (can fail if there is a conflict).
  { t.merge(arg) } -> std::same_as<HogDBStatus<void>>;
  // reverts the changes in arg that were previously merged.
  { t.undo_merge(arg) } -> std::same_as<void>;
  // returns the size in bytes of storage required in ramdb.
  { t.storage_size() } -> std::same_as<size_t>;
};

// NNInt128 commutes with other NNInt128's if base + minus >= 0 && base +
// plus does not overflow. The item's value at commit time is base + delta.
class NNInt128 {
public:
  NNInt128(int128_t base, int128_t delta) : base_(base), delta_(delta)
  {
    if (delta_ > 0) {
      plus_ = delta_;
      return;
    }
    minus_ = delta_;
  }

  HogDBStatus<void> commit(HogDatabase &db, ItemKey key) const;
  HogDBStatus<void> merge(const NNInt128 &nnint);
  void undo_merge(const NNInt128 &nnint);

  static size_t storage_size() { return sizeof(ItemKey) + sizeof(int128_t); }
  int128_t value() const { return base_ + delta_; }

private:
  int128_t base_;
  int128_t delta_;
  int128_t plus_{0};  // sum of all positive deltas (positive value)
  int128_t minus_{0}; // sum of all negative deltas (negative value)

  void assert_is_valid() const
  {
    // check for valid bounds.
    assert(minus_ <= 0);
    assert(plus_ >= 0);
    assert(minus_ <= delta_);
    assert(delta_ <= plus_);

    // check that the object is not in a conflict state.
    [[maybe_unused]] auto [ign, of] = overflowing_add(base_, plus_);
    assert(!of);
    assert(base_ + minus_ >= 0);
  }
};

static_assert(is_item<NNInt128>);

// Bytes is an item type that manages an opaque variable-length arrays of
// bytes. It commutes with other Bytes's if the array is equal.
struct Bytes {
public:
  Bytes(std::span<const std::byte> buf) : buf_(buf.begin(), buf.end()) {}

  HogDBStatus<void> commit(HogDatabase &db, ItemKey key) const;
  HogDBStatus<void> merge(const Bytes &bytes)
  {
    if (buf_ != bytes.buf_)
      return HogDBError(HogDBCode::kConflict);
    return {};
  }
  static void undo_merge(const Bytes &bytes) {}

  size_t storage_size() const { return sizeof(ItemKey) + buf_.size(); }
  OwningSpan<const std::byte> value() const
  {
    std::vector<std::byte> buf(buf_);
    auto span = std::span<const std::byte>(buf);
    return OwningSpan(span, [ref = std::move(buf)] {});
  }

private:
  std::vector<std::byte> buf_;
};

static_assert(is_item<Bytes>);

// Deletion is an item type that removes the item. It commutes with other
// Deletion's to the same item.
class Deletion {
public:
  static HogDBStatus<void> commit(HogDatabase &db, ItemKey key);
  static HogDBStatus<void> merge(const Deletion & /*del*/) { return {}; }
  static void undo_merge(const Deletion &del) {}
  static size_t storage_size() { return 0; }
};

static_assert(is_item<Deletion>);

// ItemChange represents a pending modification to an item.
class ItemChange {
public:
  ItemChange() = default;
  ItemChange(NNInt128 &&nnint) : type_(std::move(nnint)) {}
  ItemChange(Bytes &&bytes) : type_(std::move(bytes)) {}
  ItemChange(Deletion &&d) : type_(std::move(d)) {}
  ~ItemChange() = default;

  // getters
  std::optional<int128_t> to_nnint128() const;
  OwningSpan<const std::byte> to_bytes() const;
  bool is_deletion() const { return std::holds_alternative<Deletion>(type_); }

  // commit applies the item's changes to the ramdb.
  HogDBStatus<void> commit(HogDatabase &db, ItemKey key) const;

  // merge merges a change into this one. It returns kConflict if the change
  // does not commute.
  HogDBStatus<void> merge(const ItemChange &change);
  // undo_merge reverts a change from this one.
  void undo_merge(const ItemChange &change);

  // storage_size returns the bytes needed to store the item if committed.
  size_t storage_size() const;

private:
  std::variant<std::monostate, NNInt128, Bytes, Deletion> type_;
};

inline std::optional<int128_t>
ItemChange::to_nnint128() const
{
  if (const auto *tmp = std::get_if<NNInt128>(&type_); tmp)
    return tmp->value();
  return std::nullopt;
}

inline OwningSpan<const std::byte>
ItemChange::to_bytes() const
{
  if (const auto *tmp = std::get_if<Bytes>(&type_); tmp)
    return tmp->value();
  return {};
}

} // namespace hogd
