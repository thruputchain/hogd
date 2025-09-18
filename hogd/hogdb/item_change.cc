#include "hogd/hogdb/item_change.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
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
    return type_ret.error();
  }

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

HogDBStatus<void>
ItemChange::merge(const ItemChange &change)
{
  if (type_.index() != change.type_.index())
    return HogDBError(HogDBCode::kConflict);
  if (exclusive_ || change.exclusive_)
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

HogDBStatus<ssize_t>
ItemChange::storage_cost_delta(HogDBDAO &db, const ItemKey &key)
{
  // fetch the snapshot size of the object from the database if needed.
  if (!snapshot_cost_) {
    auto snapshot_cost = [&db, key] -> HogDBStatus<size_t> {
      HogDBStatus<size_t> ret = db.get_item_size(key);
      if (!ret && ret.error() == HogDBCode::kNotFound)
        return 0UL;
      if (!ret)
        return ret.error();
      return storage_cost(*ret);
    }();
    if (!snapshot_cost)
      return snapshot_cost.error();
    snapshot_cost_ = *snapshot_cost;
  }

  // calculate the delta between the size of the changed item and its snapshot.
  ssize_t cost = is_deletion() ? 0 : storage_cost(storage_size());
  return cost - static_cast<ssize_t>(*snapshot_cost_);
}

} // namespace hogd
