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

#include "account.hh"
#include "contract.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "types.hh"

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
  // Returns an upper bound of the TOTAL amount of storage tokens
  // that must be locked to support the item, after the merge.
  // THIS AMOUNT IS DETERMINISTIC (unless we implement retrocausal nnints).
  // The actual amount of storage tokens that are locked
  // will be reduced by the amount locked for the current snapshot.
  { t.merge(arg) } -> std::same_as<HogDBStatus<int128_t>>;
  // An upper bound on the amount of storage tokens needed to support the item change,
  // for the case where the change is the first one applied to the item.
  { t.marginal_storage_cost() } -> std::same_as<int128_t>;

  // reverts the change and reservation in arg that was previously merged.
  { t.undo_merge(arg) } -> std::same_as<void>;
  // returns the size in bytes of storage required in ramdb.
  { t.storage_size() } -> std::same_as<size_t>;
  // Marks a change as committed.
  { t.commit_change(arg) } -> std::same_as<void>;
  // Returns the amount of storage tokens that are actually needed to support
  // the item, for all of the changes that are currently committed.
  // The caller is responsible for releasing the difference between the amount
  // returned by merge, and the difference in this cost before and after a call
  // to commit_change. Returns std::nullopt if no changes have been committed
  // (so the cost is the cost of the snapshot item, which isn't stored inside
  // the item structs).
  { t.committed_storage_cost() } -> std::same_as<std::optional<int128_t>>;
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
  HogDBStatus<int128_t> merge(const NNInt128 &nnint);
  void undo_merge(const NNInt128 &nnint);

  static size_t storage_size() { return sizeof(int128_t); }
  int128_t value() const { return base_ + delta_; }
  int128_t base() const { return base_; }

  void commit_change(const NNInt128 & /*unused*/) { change_committed_ = true; }

  std::optional<int128_t> committed_storage_cost() const
  {
    if (change_committed_) {
      return marginal_storage_cost();
    }
    return std::nullopt;
  }

  constexpr static int128_t marginal_storage_cost()
  {
    return storage_cost(storage_size());
  }

private:
  int128_t base_;
  int128_t delta_;
  int128_t plus_{0};  // sum of all positive deltas (positive value)
  int128_t minus_{0}; // sum of all negative deltas (negative value)

  bool change_committed_{false};

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
  HogDBStatus<int128_t> merge(const Bytes &bytes)
  {
    if (buf_ != bytes.buf_)
      return HogDBError(HogDBCode::kConflict);
    return marginal_storage_cost();
  }
  static void undo_merge(const Bytes &bytes) {}

  size_t storage_size() const { return buf_.size(); }
  OwningSpan<const std::byte> value() const
  {
    std::vector<std::byte> buf(buf_);
    auto span = std::span<const std::byte>(buf);
    return OwningSpan(span, [ref = std::move(buf)] {});
  }

  void commit_change(const Bytes & /*unused*/) { change_committed_ = true; }

  std::optional<int128_t> committed_storage_cost() const
  {
    if (change_committed_) {
      return marginal_storage_cost();
    }
    return std::nullopt;
  }

  int128_t marginal_storage_cost() const
  {
    return storage_cost(storage_size());
  }

private:
  std::vector<std::byte> buf_;
  // If a change has been committed, then every subsequent change can
  // release any locked storage tokens
  bool change_committed_{false};
};

static_assert(is_item<Bytes>);

// Deletion is an item type that removes the item. It commutes with other
// Deletion's to the same item.
class Deletion {
public:
  static HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                                  const ItemKey &key);
  static bool narrow(const Deletion & /*del*/) { return true; }
  static HogDBStatus<int128_t> merge(const Deletion & /*del*/)
  {
    return marginal_storage_cost();
  }
  static void undo_merge(const Deletion &del) {}
  static size_t storage_size() { return 0; }

  void commit_change(const Deletion & /*del*/) { change_committed_ = true; }
  std::optional<int128_t> committed_storage_cost() const
  {
    if (change_committed_) {
      return marginal_storage_cost();
    }
    return std::nullopt;
  }

  constexpr static int128_t marginal_storage_cost()
  {
    return storage_cost(storage_size());
  }

  bool change_committed_{false};
};

static_assert(is_item<Deletion>);

class DeployedContractData {
public:
  DeployedContractData(DeployedContractHash deployed_contract_hash)
    : deployed_contract_hash_(deployed_contract_hash)
  {}

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key) const;
  bool narrow(const DeployedContractData &deployed_contract_data)
  {
    assert(deployed_contract_hash_ ==
           deployed_contract_data.deployed_contract_hash_);
    return true;
  }
  HogDBStatus<int128_t>
  merge(const DeployedContractData &deployed_contract_data)
  {
    assert(deployed_contract_hash_ ==
           deployed_contract_data.deployed_contract_hash_);
    return marginal_storage_cost();
  }
  static void undo_merge(const DeployedContractData &deployed_contract_data) {}
  size_t storage_size() const { return deployed_contract_hash_.contract_size; }

  const DeployedContractHash &deployed_contract_hash() const
  {
    return deployed_contract_hash_;
  }

  void commit_change(const DeployedContractData & /*deployed_contract_data*/)
  {
    change_committed_ = true;
  }
  std::optional<int128_t> committed_storage_cost() const
  {
    if (change_committed_) {
      return marginal_storage_cost();
    }
    return std::nullopt;
  }

  int128_t marginal_storage_cost() const
  {
    return storage_cost(storage_size());
  }

  bool change_committed_{false};

private:
  DeployedContractHash deployed_contract_hash_;
};

static_assert(is_item<DeployedContractData>);

class NewContractData {
public:
  NewContractData(Contract new_contract)
    : new_contract_(std::move(new_contract))
  {}

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix & /*unused*/,
                           const ItemKey &key) const;
  bool narrow(const NewContractData &new_contract_data)
  {
    assert(new_contract_ == new_contract_data.new_contract_);
    return true;
  }
  HogDBStatus<int128_t> merge(const NewContractData &new_contract_data)
  {
    assert(new_contract_ == new_contract_data.new_contract_);
    return 0;
  }
  static void undo_merge(const NewContractData &new_contract_data) {}
  static size_t storage_size()
  {
    // Storage costs for new contracts are accounted for when deploying them,
    // not in the actual contract item
    return 0;
  }

  static void commit_change(const NewContractData & /*new_contract_data*/) {};
  static std::optional<int128_t> committed_storage_cost() { return 0; }

  const Contract &new_contract() const { return new_contract_; }

  constexpr static int128_t marginal_storage_cost() { return 0; }

private:
  Contract new_contract_;
};

static_assert(is_item<NewContractData>);

// ItemChange represents a pending modification to an item.
class ItemChange {
public:
  ItemChange() = default;
  ItemChange(NNInt128 &&nnint) : type_(std::move(nnint)) {}
  ItemChange(Bytes &&bytes) : type_(std::move(bytes)) {}
  ItemChange(Deletion &&d) : type_(std::move(d)) {}
  ItemChange(DeployedContractData &&deployed_contract_data)
    : type_(std::move(deployed_contract_data))
  {}
  ItemChange(NewContractData &&new_contract_data)
    : type_(std::move(new_contract_data))
  {}
  ~ItemChange() = default;

  // getters
  std::optional<int128_t> to_nnint128() const;
  OwningSpan<const std::byte> to_bytes() const;
  bool is_deletion() const { return std::holds_alternative<Deletion>(type_); }
  bool is_exclusive() const { return exclusive_; }
  std::optional<NNInt128> get_raw_nnint128() const;
  std::optional<SHA256Hash> get_raw_deployed_contract_hash() const;

  // commit applies the item's changes to the ramdb.
  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           ItemKey key) const;

  // narrow combines the reservation of a change into this one.
  void narrow(const ItemChange &change);
  // merge merges the reservation and value of a change into this one. It
  // returns kConflict if the change does not commute.
  HogDBStatus<int128_t> merge(const ItemChange &change);
  // undo_merge reverts a change that was merged with this one.
  void undo_merge(const ItemChange &change);
  // commit_change marks a change as committed.
  // The change MUST have been previously merged.
  void commit_change(const ItemChange &change);

  int128_t marginal_storage_cost() const;
  HogDBStatus<int128_t> committed_storage_cost(HogDBDAO &db, const ItemKey &key);

  // storage_cost_delta returns the change in storage cost (in native tokens)
  // needed to store the item if committed (could be negative).
  HogDBStatus<ssize_t> storage_cost_delta(HogDBDAO &db, const ItemKey &key);

private:
  bool exclusive_{false};
  std::optional<int128_t> snapshot_cost_;
  std::variant<std::monostate, NNInt128, Bytes, Deletion, DeployedContractData,
               NewContractData>
      type_;

  size_t storage_size() const;

  HogDBStatus<void> load_snapshot_cost(HogDBDAO &db, const ItemKey &key);
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
