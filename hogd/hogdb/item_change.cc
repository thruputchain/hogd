#include "hogd/hogdb/item_change.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>
#include <variant>

#include "account.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/serialize.h"

namespace hogd {

inline HogDBStatus<void>
NNInt128::commit(HogDBDAO &db, MerkleTriePrefix &trie, const ItemKey &key) const
{
  int128_t v = value();
  HogDBStatus<void> ret = db.write_nnint128(key, v);
  if (ret) {
    std::span<std::byte> value_span(reinterpret_cast<std::byte *>(&v),
                                    sizeof(v));
    // [MerkleTrieHelpers::update_kv_pair_in_trie] will always succeed as
    // [trie.insert_or_update()] only fails if we attempt to insert an empty
    // key.
    CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, key, kNNInt128,
                                                    value_span)
              .has_value());
  }
  return ret;
}

inline bool
NNInt128::narrow(const NNInt128 &nnint)
{
  assert_is_valid();
  nnint.assert_is_valid();

  // base must be the same to narrow.
  if (base_ != nnint.base_)
    return false;

  // narrow the bounds based on the greatest restriction.
  int128_t plus = std::max(plus_, nnint.plus_);
  int128_t minus = std::min(minus_, nnint.minus_);

  // check if could go negative.
  if (base_ + minus < 0)
    return false;

  // check if could overflow.
  auto [ign, of] = overflowing_add(base_, plus);
  if (of)
    return false;

  // update bounds.
  plus_ = plus;
  minus_ = minus;

  assert_is_valid();
  return true;
}

inline HogDBStatus<int128_t>
NNInt128::merge(const NNInt128 &nnint, const StorageCostModel *cost_model)
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
  return marginal_storage_cost(cost_model);
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
Bytes::commit(HogDBDAO &db, MerkleTriePrefix &trie, const ItemKey &key) const
{
  std::span<const std::byte> v = std::as_bytes(std::span(buf_));
  HogDBStatus<void> ret = db.write_bytes(key, v);
  if (ret) {
    // [MerkleTrieHelpers::update_kv_pair_in_trie] will always succeed as
    // [trie.insert_or_update()] only fails if we attempt to insert an empty
    // key.
    CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, key, kBytes, v)
              .has_value());
  }
  return ret;
}

inline HogDBStatus<void>
Deletion::commit(HogDBDAO &db, MerkleTriePrefix &trie, const ItemKey &key)
{
  HogDBStatus<int8_t> type_ret = db.get_item_type(key);
  if (!type_ret) {
    // Deleting a non-existent key is a no-op.
    if (type_ret.error() == HogDBCode::kNotFound) {
      return {};
    }
    return type_ret.error();
  }

  info("Deletion::commit (calling remove_item)");
  HogDBStatus<void> remove_ret = db.remove_item(key);
  if (!remove_ret) {
    return remove_ret;
  }
  // [MerkleTrieHelpers::remove_kv_pair_from_trie] will always succeed as we
  // will never attempt to remove a key that does not exist. We already checked
  // above that the key exists when we removed it from the database.
  CHECK(MerkleTrieHelpers::remove_kv_pair_from_trie(trie, key, type_ret.value())
            .has_value());
  return {};
}

inline HogDBStatus<void>
DeployedContractData::commit(HogDBDAO &db, MerkleTriePrefix &trie,
                             const ItemKey &key) const
{
  assert(key.is_address_metadata());
  HogDBStatus<void> ret =
      db.write_address_metadata(key.get_address(), deployed_contract_hash_);
  if (ret) {
    auto buf = x23::serialize(deployed_contract_hash_);
    CHECK(
        MerkleTrieHelpers::update_kv_pair_in_trie(trie, key, 0,
                                                  std::as_bytes(std::span(buf)))
            .has_value());
  }
  return ret;
}

inline HogDBStatus<void>
NewContractData::commit(HogDBDAO &db, MerkleTriePrefix & /*unused*/,
                        const ItemKey &key) const
{
  auto buf = x23::serialize(new_contract_);
  return db.write_wasm_code(key.get_contract_code_hash(),
                            std::as_bytes(std::span(buf)));
}

HogDBStatus<void>
AuthDataItem::commit(HogDBDAO &db, MerkleTriePrefix &trie,
                     const ItemKey &key) const
{
  if constexpr (is_debug_build()) {
    for (size_t i = 0; i < MAX_SIGNERS; ++i) {
      assert(!pk_changes_[i].exclusive_);
    }
    assert(key.is_account_auth_data());
  }
  auto res = build_result();
  auto ret = db.write_address_account_auth_data(key.get_address(), res);
  if (!ret) {
    return ret.error();
  }
  auto buf = x23::serialize(res);
  CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, key, 0,
                                                  std::as_bytes(std::span(buf)))
            .has_value());
  return ret;
}

AccountAuthData
AuthDataItem::build_result() const
{
  AccountAuthData result = base_auth_data_;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    auto const &pk_change = pk_changes_[i];
    assert(!pk_change.exclusive_);
    if (pk_change.committed_) {
      result.public_keys[i] = pk_change.pk_;
    }
  }
  return result;
}

void
AuthDataItem::commit_change(AuthDataItem const &auth_data_item)
{
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    if (auth_data_item.pk_changes_[i].exclusive_) {
      pk_changes_[i].exclusive_ = false;
    }
    if (auth_data_item.pk_changes_[i].refcount_ > 0) {
      assert(pk_changes_[i].refcount_ > 0);
      assert(!auth_data_item.pk_changes_[i].committed_);
      pk_changes_[i].committed_ = true;
    }
  }
}

int128_t
AuthDataItem::committed_storage_cost(const StorageCostModel *cost_model) const
{
  size_t active_keys = 0;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    if (pk_changes_[i].committed_) {
      if (pk_changes_[i].pk_) {
        active_keys++;
      }
    }
    else {
      if (base_auth_data_.public_keys[i]) {
        active_keys++;
      }
    }
  }
  return cost_model->get_auth_data_cost(active_keys);
}

int128_t
AuthDataItem::marginal_storage_cost(const StorageCostModel *cost_model) const
{
  size_t new_keys_upper_bound = 0;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    if (pk_changes_[i].refcount_ > 0 || base_auth_data_.public_keys[i]) {
      new_keys_upper_bound++;
    }
  }
  return cost_model->get_auth_data_cost(new_keys_upper_bound);
}

bool
AuthDataItem::narrow(const AuthDataItem &auth_data_item)
{
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    // Narrow is only called on the changes held by individual invokeTx objects.
    assert(auth_data_item.pk_changes_[i].refcount_ == 0 ||
           auth_data_item.pk_changes_[i].refcount_ == 1);
    if (auth_data_item.pk_changes_[i].refcount_ == 1) {
      pk_changes_[i].refcount_ = 1;
    }
    pk_changes_[i].exclusive_ |= auth_data_item.pk_changes_[i].exclusive_;
  }
  return true;
}

HogDBStatus<int128_t>
AuthDataItem::merge(const AuthDataItem &auth_data_item,
                    const StorageCostModel *cost_model)
{
  auto marginal_cost = marginal_storage_cost(cost_model);
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    // if merging this pk change requires a lock or updating the change
    assert(auth_data_item.pk_changes_[i].refcount_ == 0 ||
           auth_data_item.pk_changes_[i].refcount_ == 1);

    if (auth_data_item.pk_changes_[i].refcount_ == 0) {
      assert(!auth_data_item.pk_changes_[i].exclusive_);
      continue;
    }

    if (pk_changes_[i].refcount_ == 0 &&
        auth_data_item.pk_changes_[i].refcount_ == 1) {
      assert(pk_changes_[i].exclusive_ == false);
      continue;
    }

    // both have nonzero refcount
    if (pk_changes_[i].exclusive_) {
      assert(!auth_data_item.pk_changes_[i].exclusive_);
      return HogDBError(HogDBCode::kConflict);
    }

    if (pk_changes_[i].pk_ != auth_data_item.pk_changes_[i].pk_) {
      return HogDBError(HogDBCode::kConflict);
    }
  }

  // Conflicts checked; now do merge
  size_t max_new_keys = 0;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    pk_changes_[i].refcount_ += auth_data_item.pk_changes_[i].refcount_;

    if (auth_data_item.pk_changes_[i].refcount_ == 1) {
      pk_changes_[i].pk_ = auth_data_item.pk_changes_[i].pk_;
    }
    if (auth_data_item.pk_changes_[i].exclusive_) {
      pk_changes_[i].exclusive_ = true;
    }

    // mirroring the logic in marginal_storage_cost
    if (auth_data_item.pk_changes_[i].refcount_ == 1 ||
        base_auth_data_.public_keys[i]) {
      max_new_keys++;
    }
  }

  return cost_model->get_auth_data_cost(max_new_keys);
}

void
AuthDataItem::undo_merge(const AuthDataItem &auth_data_item)
{
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    assert(auth_data_item.pk_changes_[i].refcount_ == 0 ||
           auth_data_item.pk_changes_[i].refcount_ == 1);
    pk_changes_[i].refcount_ -= auth_data_item.pk_changes_[i].refcount_;
    if (auth_data_item.pk_changes_[i].exclusive_) {
      assert(pk_changes_[i].exclusive_);
      pk_changes_[i].exclusive_ = false;
    }
  }
}

namespace {

template<typename T>
concept is_monostate = std::same_as<std::decay_t<T>, std::monostate>;

} // namespace

HogDBStatus<void>
ItemChange::commit(HogDBDAO &db, MerkleTriePrefix &trie, ItemKey key) const
{
  return std::visit(
      [&db, &trie, &key](auto &arg) -> HogDBStatus<void> {
        if constexpr (!is_monostate<decltype(arg)>)
          return arg.commit(db, trie, key);
        return {};
      },
      type_);
}

void
ItemChange::narrow(const ItemChange &change)
{
  if (type_.index() != change.type_.index() || change.exclusive_) {
    exclusive_ = true;
    return;
  }

  exclusive_ = !std::visit(
      [&change](auto &arg) -> bool {
        if constexpr (!is_monostate<decltype(arg)>)
          return arg.narrow(
              std::get<std::decay_t<decltype(arg)>>(change.type_));
        return {};
      },
      type_);
}

HogDBStatus<int128_t>
ItemChange::merge(const ItemChange &change, const StorageCostModel *cost_model)
{
  if (type_.index() != change.type_.index())
    return HogDBError(HogDBCode::kConflict);
  if (exclusive_ || change.exclusive_)
    return HogDBError(HogDBCode::kConflict);

  auto merge_cost = std::visit(
      [&change, cost_model](auto &arg) -> HogDBStatus<int128_t> {
        if constexpr (!is_monostate<decltype(arg)>)
          return arg.merge(std::get<std::decay_t<decltype(arg)>>(change.type_),
                           cost_model);
        assert(false);
        return HogDBError(HogDBCode::kInternal);
      },
      type_);
  if (!merge_cost) {
    return merge_cost.error();
  }
  assert(snapshot_cost_.has_value());
  return std::max<int128_t>(0, *merge_cost - *snapshot_cost_);
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

void
ItemChange::commit_change(const ItemChange &change)
{
  assert(type_.index() == change.type_.index());
  // In the future, we will forward the change
  // to the variant.
  if (change.exclusive_) {
    // Committing the change that held the exclusive lock allows
    // us to release that lock.
    exclusive_ = false;
  }
  change_committed_ = true;
  std::visit(
      [&change](auto &arg) {
        if constexpr (is_item_commit_required<std::decay_t<decltype(arg)>>)
          arg.commit_change(
              std::get<std::decay_t<decltype(arg)>>(change.type_));
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

HogDBStatus<int128_t>
ItemChange::marginal_storage_cost(HogDBDAO &db, const ItemKey &key,
                                  const StorageCostModel *cost_model)
{

  if (!snapshot_cost_) {
    auto ret = load_snapshot_cost(db, key, cost_model);
    if (!ret)
      return ret.error();
  }

  return marginal_storage_cost(cost_model);
}

HogDBStatus<int128_t>
ItemChange::marginal_storage_cost(const StorageCostModel *cost_model) const
{
  assert(snapshot_cost_.has_value());
  auto marginal_item = std::visit(
      [cost_model](auto &arg) -> HogDBStatus<int128_t> {
        if constexpr (!is_monostate<decltype(arg)>)
          return arg.marginal_storage_cost(cost_model);
        assert(false);
        return HogDBError(HogDBCode::kInternal);
      },
      type_);
  if (!marginal_item)
    return marginal_item.error();
  return std::max<int128_t>(0, *marginal_item - *snapshot_cost_);
}

int128_t
ItemChange::committed_storage_cost(const StorageCostModel *cost_model) const
{
  assert(snapshot_cost_.has_value());
  if (!change_committed_) {
    return *snapshot_cost_;
  }

  return std::visit(
      [cost_model](auto &arg) -> int128_t {
        assert(!is_monostate<decltype(arg)>);

        if constexpr (!is_monostate<decltype(arg)>)
          return arg.committed_storage_cost(cost_model);
        assert(false);
        return std::numeric_limits<int128_t>::max();
      },
      type_);
}

HogDBStatus<int128_t>
ItemChange::committed_storage_cost(HogDBDAO &db, const ItemKey &key,
                                   const StorageCostModel *cost_model)
{
  if (!snapshot_cost_) {
    auto ret = load_snapshot_cost(db, key, cost_model);
    if (!ret)
      return ret.error();
  }
  return committed_storage_cost(cost_model);
}

HogDBStatus<void>
ItemChange::load_snapshot_cost(HogDBDAO &db, const ItemKey &key,
                               StorageCostModel const *cost_model)
{
  auto snapshot_cost = [&db, key, cost_model] -> HogDBStatus<size_t> {
    HogDBStatus<size_t> ret = db.get_charged_storage_cost(key, cost_model);
    if (!ret && ret.error() == HogDBCode::kNotFound) {
      // item does not exist in the snapshot
      return 0UL;
    }
    if (!ret)
      return ret.error();

    return *ret;
  }();
  if (!snapshot_cost)
    return snapshot_cost.error();
  snapshot_cost_ = *snapshot_cost;
  return {};
}

} // namespace hogd
