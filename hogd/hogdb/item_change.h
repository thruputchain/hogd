// item_change.h - modification tracking for DB items.
//
// Design Overview:
//
// Each ItemChange tracks modifications to an item. Conceptually, an ItemChange
// has two components: a reservation and a change. The reservation defines what
// should cause conflicts with other changes while the change modifies the
// item's value if committed. Reservations can be combined into their most
// restrictive intersection with narrow(). On the other hand, both reservations
// and changes can be combined together with merge().
//
// This distinction is useful because subtransactions must contain reservations
// that are at least as restrictive as the parent transaction for any given
// item. Otherwise, rolling back could fail because of a lock taken by another
// transaction. Therefore the reservation might have to be more restrictive
// than the change.

#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/perthread.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"

namespace hogd {

// is_item is a concept that defines the required methods for a class that
// tracks a change to a DB item. Each class that implements this concept is an
// item type.
template<typename T>
concept is_item = requires(T t, HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key, const T &arg) {
  // applies the change to the ramdb.
  { t.commit(db, trie, key) } -> std::same_as<HogDBStatus<void>>;
  // makes the change's reservation at least as restrictive as arg. However,
  // does not merge the value change in arg. Returns true if narrowing was
  // successful.
  { t.narrow(arg) } -> std::same_as<bool>;
  // merges the change and reservation in arg (will fail if there is a
  // conflict).
  { t.merge(arg) } -> std::same_as<HogDBStatus<void>>;
  // reverts the change and reservation in arg that was previously merged.
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

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key) const;
  bool narrow(const NNInt128 &nnint);
  HogDBStatus<void> merge(const NNInt128 &nnint);
  void undo_merge(const NNInt128 &nnint);

  static size_t storage_size() { return sizeof(int128_t); }
  int128_t value() const { return base_ + delta_; }
  int128_t base() const { return base_; }

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

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key) const;
  bool narrow(const Bytes &bytes) { return buf_ == bytes.buf_; }
  HogDBStatus<void> merge(const Bytes &bytes)
  {
    if (buf_ != bytes.buf_)
      return HogDBError(HogDBCode::kConflict);
    return {};
  }
  static void undo_merge(const Bytes &bytes) {}

  size_t storage_size() const { return buf_.size(); }
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
  static HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                                  const ItemKey &key);
  static bool narrow(const Deletion & /*del*/) { return true; }
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
  bool is_exclusive() const { return exclusive_; }
  std::optional<NNInt128> get_raw_nnint128() const;

  // setters
  void set_snapshot_cost(size_t val) { snapshot_cost_ = val; }

  // commit applies the item's changes to the ramdb.
  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           ItemKey key) const;

  // narrow combines the reservation of a change into this one.
  void narrow(const ItemChange &change);
  // merge merges the reservation and value of a change into this one. It
  // returns kConflict if the change does not commute.
  HogDBStatus<void> merge(const ItemChange &change);
  // undo_merge reverts a change that was merged with this one.
  void undo_merge(const ItemChange &change);

  // storage_cost_delta returns the change in storage cost (in native tokens)
  // needed to store the item if committed (could be negative).
  HogDBStatus<ssize_t> storage_cost_delta(HogDBDAO &db, const ItemKey &key);

private:
  bool exclusive_{false};
  std::optional<size_t> snapshot_cost_;
  std::variant<std::monostate, NNInt128, Bytes, Deletion> type_;

  size_t storage_size() const;
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

inline std::optional<NNInt128>
ItemChange::get_raw_nnint128() const
{
  if (const auto *tmp = std::get_if<NNInt128>(&type_); tmp)
    return *tmp;
  return std::nullopt;
}

inline HogDBStatus<ItemChange>
make_balance_change(HogDBDAO &db, const Address &addr, int128_t delta)
{
  auto balance = db.read_address_balance(addr);
  if (!balance.has_value()) {
    if (balance.error() != HogDBCode::kNotFound) {
      return balance.error();
    }
    balance = 0;
  }
  return ItemChange(NNInt128(*balance, delta));
}

} // namespace hogd
