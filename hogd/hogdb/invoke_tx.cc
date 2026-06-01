#include "hogd/hogdb/invoke_tx.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <expected>
#include <optional>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "contract.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "hogexec_constants.hh"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/serialize.h"

namespace hogd {

HogDBStatus<int128_t>
InvokeTX::read_nnint128(const ItemKey &key)
{
  const ItemChange *change = read_change(key);
  if (!change)
    return block_.get().get_db().read_nnint128(key);
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
    return block_.get().get_db().read_bytes(key);
  if (change->is_deletion())
    return HogDBError(HogDBCode::kNotFound);
  OwningSpan<const std::byte> ret = change->to_bytes();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return ret;
}

HogDBStatus<Balance>
InvokeTX::get_storage_balance_change(Address const &addr) const
{
  auto bal_it = balance_deltas_.find(addr);

  int128_t delta = 0;
  if (bal_it != balance_deltas_.end() && !bal_it->second.empty()) {
    auto const &bal = bal_it->second;
    auto [ign, of] = overflowing_sub(bal.back().manual_transfer_amount,
                                     bal.back().total_locked_storage_amount);
    if (of) {
      if constexpr (is_debug_build()) {
        ContractAddress contract_addr = addr.get_raw();
        trace(
            "subtraction overflow when computing balance change for address {}",
            x23::show(contract_addr));
      }
      return HogDBError(HogDBCode::kOverflow);
    }
    delta = ign;
  }

  return Balance(delta);
}

// Is only responsible for updating the lock on the balance item;
// not for updating anything else
HogDBStatus<void>
InvokeTX::update_balance_lock(Address const &addr)
{
  // Combines the manual balance transfers with the up-to-now storage cost
  // to create/apply/update one balance nnint delta.
  auto storage_change_res = get_storage_balance_change(addr);
  if (!storage_change_res) {
    return storage_change_res.error();
  }
  auto storage_change = storage_change_res.value();
  auto new_item = ItemChange(std::move(storage_change));

  auto key = make_address_balance_item_key(addr);
  auto &change = changes_[key];
  if (change.empty()) {

    auto status = block_.get().lock(key, new_item);
    if (!status) {
      if constexpr (is_debug_build()) {
        ContractAddress contract_addr = addr.get_raw();
        assert(new_item.is_balance());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        trace("failed to lock balance item for address {}: {}",
              x23::show(contract_addr), *new_item.to_balance());
        // NOLINTEND(bugprone-unchecked-optional-access)
      }
      return status.error();
    }

    change.emplace_back(std::move(new_item), tx_depth_);
    return {};
  }

  auto const &last = change.back();
  assert(last.tx_depth <= tx_depth_);
  assert(last.change.is_balance());
  if (auto r = new_item.narrow(last.change); !r)
    return r.error();

  assert(last.change.is_balance());
  assert(new_item.is_balance());
  assert(new_item.to_balance());
  assert(last.change.to_balance());

  // Check if the new item is the same as the previous storage lock.
  // If so, no need to update the lock.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  if (*last.change.to_balance() == *new_item.to_balance()) {
    return {};
  }

  auto status = block_.get().modify_lock(key, last.change, new_item);
  if (!status) {
    if constexpr (is_debug_build()) {
      ContractAddress contract_addr = addr.get_raw();
      // NOLINTBEGIN(bugprone-unchecked-optional-access)
      auto i = last.change.get_raw_nnint128();
      if (i)
        trace("failed to modify lock for address {}: {}",
              x23::show(contract_addr), *i);
      else
        trace("failed to modify lock for address {}: nullopt",
              x23::show(contract_addr));
      // NOLINTEND(bugprone-unchecked-optional-access)
    }
    return status.error();
  }
  assert(last.tx_depth < tx_depth_);
  change.emplace_back(std::move(new_item), tx_depth_);
  return {};
}

void
InvokeTX::lock_storage(Address const &addr, int128_t cost)
{
  auto &bal_stats = balance_deltas_[addr];
  if (bal_stats.empty()) {
    bal_stats.emplace_back(
        StorageBalanceVersion{.manual_transfer_amount = 0,
                              .total_locked_storage_amount = 0,
                              .tx_depth = tx_depth_});
  }
  else if (bal_stats.back().tx_depth < tx_depth_) {
    // setup the back with a copy of the current state
    auto back = bal_stats.back();
    bal_stats.emplace_back(StorageBalanceVersion{
        .manual_transfer_amount = back.manual_transfer_amount,
        .total_locked_storage_amount = back.total_locked_storage_amount,
        .tx_depth = tx_depth_});
  }
  assert(!bal_stats.empty());
  assert(bal_stats.back().tx_depth == tx_depth_);

  bal_stats.back().total_locked_storage_amount += cost;
}

HogDBStatus<void>
InvokeTX::commit()
{
  assert_is_valid();

  // Phase 1: commit balance keys first, finalizing their locks so that
  // release_balance (called in phase 2) can apply post-commit credit.
  // TODO(geoff): Balance keys currently never produce refunds
  // (BalanceLock::commit_change returns 0, locked_storage_cost is 0).
  // Account deletion will break this invariant — when it lands, this
  // loop will need refund handling and release_balance will need to
  // handle the case where the balance lock is not yet finalized.
  HogDBError commit_error{HogDBCode::kInternal};
  bool failed = false;

  for (auto &[key, vec] : changes_) {
    if (!vec.empty() && key.is_address_balance()) {
      auto cost_change =
          block_.get().commit_change(key, std::move(vec.back().change));
      if (!cost_change.has_value()) {
        commit_error = cost_change.error();
        failed = true;
        break;
      }
      CHECK_EQ(*cost_change, 0);
      CHECK_EQ(vec.back().locked_storage_cost, 0);
    }
  }

  // Phase 2: commit non-balance keys. These may produce storage refunds
  // that are credited to the (now-finalized) balance locks.
  if (!failed) {
    for (auto &[key, vec] : changes_) {
      if (!vec.empty() && !key.is_address_balance()) {
        auto cost_change =
            block_.get().commit_change(key, std::move(vec.back().change));
        if (!cost_change.has_value()) {
          commit_error = cost_change.error();
          failed = true;
          break;
        }
        CHECK_LE(*cost_change, vec.back().locked_storage_cost);
        // NONDETERMINISTIC: Compute the amount of storage tokens that can be
        // released after this transaction commits.
        int128_t released_cost = vec.back().locked_storage_cost - *cost_change;
        CHECK_GE(released_cost, 0);

        if (released_cost > 0) {
          block_.get().release_balance(key.get_address(), released_cost);
        }
      }
    }
  }

  changes_.clear();
  balance_deltas_.clear();

  if (failed) {
    block_.get().poison();
    return commit_error;
  }
  return {};
}

void
InvokeTX::abort()
{
  assert_is_valid();

  for (auto &[key, vec] : changes_) {
    if (!vec.empty()) {
      block_.get().unlock(key, vec.back().change);
    }
  }
  changes_.clear();
  balance_deltas_.clear();
}

void
InvokeTX::rollback_to_savepoint(int depth)
{
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
      [[maybe_unused]] HogDBStatus<int128_t> ret =
          block_.get().modify_lock(key, tmp, vec.back().change);
      assert(ret);
      assert(vec.back().locked_storage_cost >= *ret);
      continue;
    }
    block_.get().unlock(key, tmp);
  }

  for (auto &[addr, vec] : balance_deltas_) {
    vec.erase(std::ranges::upper_bound(vec, tx_depth_, {},
                                       &StorageBalanceVersion::tx_depth),
              vec.end());
  }
  assert_is_valid();
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
    ItemVersion tmp(std::move(vec.back().change), tx_depth_);
    vec.erase(std::ranges::upper_bound(vec, tx_depth_, {},
                                       &ItemVersion::tx_depth),
              vec.end());
    if (!vec.empty() && vec.back().tx_depth == tx_depth_) {
      vec.back().change = std::move(tmp.change);
      // TODO(geoff): abelay had left this as return; that seems incorrect.
      continue;
    }
    vec.push_back(std::move(tmp));
  }

  throw std::runtime_error("not implemented releasing balance versions");
}

void
InvokeTX::request_balance_change(Address const &addr, int128_t delta)
{
  auto &bal = balance_deltas_[addr];
  if (bal.empty()) {
    bal.emplace_back(StorageBalanceVersion{.manual_transfer_amount = delta,
                                           .total_locked_storage_amount = 0,
                                           .tx_depth = tx_depth_});
  }
  else if (bal.back().tx_depth < tx_depth_) {
    bal.emplace_back(
        StorageBalanceVersion{.manual_transfer_amount = delta,
                              .total_locked_storage_amount =
                                  bal.back().total_locked_storage_amount,
                              .tx_depth = tx_depth_});
  }
  else {
    assert(bal.back().tx_depth == tx_depth_);
    bal.back().manual_transfer_amount = delta;
  }

  assert(!bal.empty());
  assert(bal.back().tx_depth == tx_depth_);
}

HogDBStatus<void>
InvokeTX::request_change(const ItemKey &key, ItemChange &&change)
{
  assert(!key.is_address_balance());
  std::vector<ItemVersion> &v = changes_[key];

  // this is the first change for this key; try to acquire a lock on the item.
  if (v.empty()) {
    HogDBStatus<int128_t> ret = block_.get().lock(key, change);
    if (!ret) {
      return ret.error();
    }

    assert(*ret >= 0 || change.is_permanent_exclusive());
    int128_t marginal_storage = *ret;

    v.emplace_back(std::move(change), tx_depth_);
    // now lock the storage
    if (marginal_storage != 0) {
      lock_storage(key.get_address(), marginal_storage);
      v.back().locked_storage_cost = marginal_storage;
    }

    return {};
  }

  // otherwise it is an existing change, so the item is already locked.
  ItemVersion &item = v.back();
  if (auto r = change.narrow(item.change); !r)
    return r.error();
  HogDBStatus<int128_t> ret =
      block_.get().modify_lock(key, item.change, change);
  if (!ret) {
    return ret.error();
  }

  int128_t previous_locked_storage = item.locked_storage_cost;

  assert(*ret >= 0 || change.is_permanent_exclusive());
  int128_t marginal_storage = *ret - previous_locked_storage;

  if (item.tx_depth == tx_depth_) {
    item.change = std::move(change);
  }
  else {
    assert(item.tx_depth < tx_depth_);
    v.emplace_back(std::move(change), tx_depth_, previous_locked_storage);
  }

  // It's ok if marginal storage is negative;
  // that means we're releasing storage costs relative to the prior locked
  // storage. Note that locking a positive value in a balance does NOT make that
  // balance immediately available to other transactions, so this is safe to do.
  // Additionally, the NNInt item preserves (via narrow()) the invariant that
  // we'll always be able to rollback to a prior saved state, so releasing
  // storage costs that were locked earlier in the tx is safe. Because *ret >=
  // 0, we won't ever release storage costs that we didn't lock up in this
  // transaction.

  // now to lock the storage
  if (marginal_storage != 0) {
    lock_storage(key.get_address(), marginal_storage);
    v.back().locked_storage_cost = *ret;
  }

  return {};
}

HogDBStatus<void>
InvokeTX::request_nnint128(const ItemKey &key, int128_t base, int128_t delta,
                           StorageDeltaFlagSet flags)
{
  assert(!key.is_address_balance());
  // validate the nnint128. We can't assume it's in a valid state even if there
  // is not a conflict with another change because the executor may not know
  // the current base value.
  auto [sum, of] = overflowing_add(base, delta);
  if (of) [[unlikely]] {
    return HogDBError(HogDBCode::kOverflow);
  }
  if (sum < std::min<int128_t>(base, 0)) {
    return HogDBError(HogDBCode::kConflict);
  }

  // nnint128 is valid; now try to request the change.
  auto change = ItemChange(NNInt128{base, delta});
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    change.set_permanent_exclusive();
  }
  return request_change(key, std::move(change));
}

HogDBStatus<void>
InvokeTX::request_nnint128(const ItemKey &key, int128_t delta,
                           StorageDeltaFlagSet flags)
{
  if (key.is_address_balance()) {
    if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
      return HogDBError(HogDBCode::kConflict);
    }
    request_balance_change(key.get_address(), delta);
    return {};
  }
  assert(!key.is_address_balance());

  HogDBStatus<int128_t> base = block_.get().read_nnint128_base(key);
  if (!base) {
    assert(base.error() != HogDBCode::kTypeMismatch &&
           base.error() != HogDBCode::kNotFound);
    return base.error();
  }
  return request_nnint128(key, *base, delta, flags);
}

HogDBStatus<void>
InvokeTX::request_auth_data(Address const &addr,
                            const AuthDataChangeArgs &auth_data_change,
                            StorageDeltaFlagSet flags)
{
  auto base_auth_data = block_.get().get_db().read_address_auth_data(addr);
  if (!base_auth_data) {
    if (base_auth_data.error() != HogDBCode::kNotFound) {
      return base_auth_data.error();
    }
    base_auth_data = AddressAuthData{};
  }

  AuthDataItem auth_data_item(std::move(base_auth_data.value()));
  for (const auto &signer : auth_data_change.auth_data_change_signers) {
    if (signer.public_key_id >= kMaxSigners) {
      return HogDBError(HogDBCode::kConflict);
    }
    auth_data_item.set_new_pk(signer.public_key_id, signer.public_key);
  }
  if (auth_data_change.new_required_weight) {
    auth_data_item.set_new_required_weight(
        *auth_data_change.new_required_weight);
  }
  auto change = ItemChange(std::move(auth_data_item));
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    change.set_permanent_exclusive();
  }
  // request_change can fail with kConflict (lock contention from another tx
  // in the same block) or kStaleBlock (a sibling block committed and changed
  // the auth base out from under us, detected in AuthDataItem::narrow()).
  auto ret =
      request_change(make_address_auth_data_item_key(addr), std::move(change));
  if (!ret) {
    CHECK(ret.error().code() == HogDBCode::kStaleBlock ||
          ret.error().code() == HogDBCode::kConflict);
  }
  return ret;
}

HogDBStatus<void>
InvokeTX::request_consume_uniquifier(Address const &addr, Uniquifier uniquifier,
                                     StorageDeltaFlagSet flags)
{
  // We do not support setting exclusive locks on replay cache items;
  // consuming a nonce is already a unique event.
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    return HogDBError(HogDBCode::kConflict);
  }

  return request_change(make_replay_cache_item_key(addr),
                        ReplayCacheIt{uniquifier});
}

HogDBStatus<void>
InvokeTX::request_retrocausal_queue(const ItemKey &key,
                                    const RetrocausalQueueDelta &delta,
                                    StorageDeltaFlagSet flags)
{
  if (delta.insert.added_entries.size() > kRetrocausalQueueBlockEntriesMaxLen) {
    return std::unexpected(HogDBError{HogDBCode::kInvalidExecutorBehavior});
  }
  auto change = ItemChange(RetrocausalQueueIt(delta));
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    change.set_permanent_exclusive();
  }
  return request_change(key, std::move(change));
}

HogDBStatus<void>
InvokeTX::sync_balance_locks()
{
  if constexpr (is_debug_build()) {
    std::vector<const Address *> addrs;
    for (const auto &[addr, vec] : balance_deltas_) {
      if (!vec.empty() && vec.back().tx_depth == tx_depth_) {
        addrs.push_back(&addr);
      }
    }
    std::shuffle(addrs.begin(), addrs.end(),
                 std::mt19937{std::random_device{}()});
    for (const auto *addr : addrs) {
      auto status = update_balance_lock(*addr);
      if (!status) {
        return status.error();
      }
    }
  }
  else {
    for (const auto &[addr, vec] : balance_deltas_) {
      if (!vec.empty() && vec.back().tx_depth == tx_depth_) {
        auto status = update_balance_lock(addr);
        if (!status) {
          return status.error();
        }
      }
    }
  }

  assert_is_valid();
  return {};
}

void
InvokeTX::assert_is_valid() const noexcept
{
  assert(tx_depth_ >= 0);
  assert(tx_depth_ <= kMaxSavepointDepth);
  if constexpr (is_debug_build()) {
    std::unordered_map<Address, int128_t> balance_deltas;
    for (const auto &[key, vec] : changes_) {
      [[maybe_unused]] unsigned int depth = 0;
      for (const ItemVersion &item : vec) {
        assert(item.tx_depth >= depth);
        assert(item.tx_depth <= tx_depth_);
        depth = item.tx_depth + 1;
      }
      if (!vec.empty()) {
        balance_deltas[key.get_address()] += vec.back().locked_storage_cost;
      }
    }
    for (const auto &[addr, vec] : balance_deltas_) {
      if (!vec.empty()) {
        assert(vec.back().total_locked_storage_amount == balance_deltas[addr]);
      }
      else {
        assert(balance_deltas[addr] == 0);
      }
    }

    // Verify that balance changes in changes_ match what's expected from
    // balance_deltas_
    for (const auto &[key, vec] : changes_) {
      if (key.is_address_balance() && !vec.empty()) {
        const ItemVersion &item = vec.back();
        assert(item.change.is_balance());
        [[maybe_unused]]
        auto balance = item.change.to_balance();
        assert(balance);

        Address addr = key.get_address();
        auto bal_it = balance_deltas_.find(addr);
        if (bal_it != balance_deltas_.end() && !bal_it->second.empty()) {
          const auto &bal = bal_it->second.back();
          [[maybe_unused]]
          const int128_t expected_delta =
              bal.manual_transfer_amount - bal.total_locked_storage_amount;
          assert(balance->delta() == expected_delta);
        }
        else {
          // No balance_deltas_ entry means no storage costs or transfers
          assert(balance->delta() == 0);
        }
      }
    }

    // Verify the reverse: for every address in balance_deltas_, there should
    // be a corresponding balance item in changes_ with the correct delta
    for (const auto &[addr, vec] : balance_deltas_) {
      if (!vec.empty()) {
        auto key = make_address_balance_item_key(addr);
        auto it = changes_.find(key);
        if (it == changes_.end() || it->second.empty()) {
          [[maybe_unused]]
          auto const &bal = vec.back();
          assert(bal.manual_transfer_amount - bal.total_locked_storage_amount ==
                 0);
          continue;
        }
        assert(it != changes_.end());
        assert(!it->second.empty());

        const ItemVersion &item = it->second.back();
        assert(item.change.is_balance());
        [[maybe_unused]]
        auto balance = item.change.to_balance();
        assert(balance);

        const auto &bal = vec.back();
        [[maybe_unused]]
        const int128_t expected_delta =
            bal.manual_transfer_amount - bal.total_locked_storage_amount;
        assert(balance->delta() == expected_delta);
      }
    }
  }
}

HogDBStatus<void>
InvokeTX::request_new_contract(Contract contract)
{
  // Reject an oversized contract here, per transaction: the wasm code row (the
  // serialized contract) is written to RAMDB verbatim, so an item exceeding
  // the size limit must fail this tx rather than the whole block at write-back,
  // where apply() leaves earlier writes visible and the commit error aborts the
  // node (#1775). hash is computed outside make_contract_code_item_key to dodge
  // an observed gcc-CI miscompile.
  auto hash = sha256(contract);
  // xdr_size computes the serialized length without allocating; it equals the
  // x23::serialize(contract) byte count NewContractLock writes for the code
  // row.
  if (!Database::item_fits(make_wasm_code_key(hash).size(),
                           x23::xdr_size(contract)))
    return HogDBError(HogDBCode::kTooBig);

  return request_change(make_contract_code_item_key(hash),
                        NewContractData(std::move(contract)));
}

} // namespace hogd
