#pragma once

#include <iostream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "db_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/mempool/shared.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/x23/async.h"
#include "x23/x23/corosync.h"
#include "x23/x23/vec.h"

namespace hogd {

class Scheduler {
public:
  Scheduler(std::shared_ptr<db_sched_client_t> db_sched_client)
    : db_sched_client_(std::move(db_sched_client))
  {}

  static x23::Async<void> shut_down() { co_return; }

  // Adds txs to the scheduler. Adding the same tx multiple times is okay; the
  // scheduler detects and silently drops the duplicates rather than enqueue
  // them.
  //
  // This method will cycle the groups for an account if doing so gives the
  // account an advantage in scheduling. This happens when (1) cycling increases
  // the average age of the [kHogAgeGroupToSchedule] group or (2) the
  // [kHogAgeGroupToSchedule] group does not have enough hogs to schedule the
  // tx but cycling will add enough hogs to this group to schedule it.
  x23::Async<void> add_txs(const x23::Vec<SignedTransaction> &txs);

  x23::Async<void> process_transfers(const HogsTransferred &transfer);

  // Chooses txs to schedule in the next block. [block_height] is the height of
  // the block that is being scheduled. [block_size] is the maximum number of
  // txs that will be included in the next block, though fewer txs than
  // [block_size] may be included.
  void schedule(size_t num_txs, x23::Vec<SignedTransactionHash> &tx_hashes,
                x23::Vec<SignedTransaction> &txs);

  // Finds the txs specified in [hashes] and removes them from the runqueue.
  // This is used when validating a block as the txs have supposedly been
  // committed to a block already, so we do not want to schedule the txs. But
  // the validator still needs to get the tx state because it needs to
  // re-execute the txs to verify the block.
  SignedTransaction *find_tx(const SignedTransactionHash &tx_hash)
  {
    auto [tx_value_ptr, _] = txs_.find(tx_hash);
    return &tx_value_ptr->tx;
  }

  void reenqueue_txs(const auto &begin, const auto &end)
  {
    for (auto it = begin; it != end; it++) {
      const SignedTransactionHash &tx_hash = *it;
      const auto [tx_value_ptr, lock] = txs_.find(tx_hash);
      CHECK_PTR(tx_value_ptr);
      CHECK(insert_tx_into_runqueue(tx_hash, *tx_value_ptr));
    }
    size_.fetch_add(std::distance(begin, end), std::memory_order_release);
  }

  void remove_from_scheduler(x23::span<const SignedTransactionHash> hashes);

  // We are exposing the functions below for testing. They should not be called
  // directly by non-test code.

  std::pair<HogAge *, std::unique_lock<std::mutex>>
  get_cached_hog_age(const ContractAddress &address);

  x23::Async<void>
  read_average_hog_ages(const x23::Vec<ContractAddress> &accounts);
  x23::Async<std::unordered_set<ContractAddress>>
  read_average_hog_ages(const x23::Vec<SignedTransaction> &txs);
  x23::Async<std::unordered_set<ContractAddress>>
  read_average_hog_ages(const HogsTransferred &transfers);

  // Moves the hogs between groups as specified by the scheduling protocol.
  // Specifically:
  // 1. Hogs in the [kHogAgeGroupAlreadyScheduled] group are moved to the
  // [kHogAgeGroupToSchedule] group.
  // 2. Then, hogs in the [kHogAgeGroupReceived] group are moved to the
  // [kHogAgeGroupAlreadyScheduled] group.
  // So this has the visual effect of:
  //     [kHogAgeGroupReceived]
  // --> [kHogAgeGroupAlreadyScheduled]
  // --> [kHogAgeGroupToSchedule].
  //
  // Note that the average ages of these groups change when hogs are moved
  // between them.
  static void cycle_groups(HogAge &current_averages);

  void increment_current_block() { current_block_++; }
  void decrement_current_block() { current_block_--; }

  // The number of txs in the scheduler, including both those of the runqueue
  // and those that are stranded.
  size_t size() const { return size_.load(std::memory_order_relaxed); }

  // Debug.
  void dump_runqueue();

  x23::CoroSem transfer_finished_;
  x23::CoroLock global_lock_;

private:
  struct TxKey {
    BlockHeight expiry_block_height;
    SignedTransactionHash tx_hash;

    bool operator<(const TxKey &other) const noexcept
    {
      if (expiry_block_height != other.expiry_block_height) {
        return expiry_block_height < other.expiry_block_height;
      }
      // std::array defines lexicographic comparison.
      return tx_hash < other.tx_hash;
    }
  };

  struct TxValue {
    // The tx itself.
    SignedTransaction tx;
  };

  struct AccountKey {
    BlockHeight average_hog_age;
    ContractAddress account;

    bool operator<(const AccountKey &other) const noexcept
    {
      if (average_hog_age != other.average_hog_age) {
        return average_hog_age < other.average_hog_age;
      }
      // std::array defines lexicographic comparison.
      return account < other.account;
    }
  };

  AccountKey get_account_key(const ContractAddress &account);

  bool account_is_ready(const ContractAddress &account,
                        bool remove_expired_txs);

  // Inserts the tx with [tx_hash] into the runqueue. Returns true if the
  // insertion succeeded and false otherwise.
  //
  // This method does *not* modify [size_]! The caller must do this. Generally,
  // the caller will often want to add multiple txs to the runqueue, so it is
  // more efficient for the caller to do a single [fetch_add()] to [size_]
  // rather than have [insert_tx_into_runqueue()] increment [size_] for every
  // single tx.
  bool insert_tx_into_runqueue(const SignedTransactionHash &tx_hash,
                               TxValue &tx_value);
  bool add_txs_helper(const SignedTransaction &tx);

  // TODO(jhumphri): Give this a better name.
  void cycle_accounts_during_schedule_helper(
      const std::unordered_set<ContractAddress> &accounts,
      const TxKey &lower_bound);

  void process_transfers_helper(const Delta &delta, BlockHeight block_height);

  static void move_hogs_to_already_scheduled_group(HogAge &hog_ages,
                                                   uint128_t num_hogs,
                                                   BlockHeight block_height);

  // This method will cycle the groups for [account] if doing so gives the
  // account an advantage in scheduling. This happens when (1) cycling increases
  // the average age of the [kHogAgeGroupToSchedule] group or (2) no txs are
  // currently on the runqueue for [account] and the [kHogAgeGroupToSchedule]
  // group does not have enough hogs to schedule a tx with a gas limit of
  // [gas_limit].
  void cycle_if_needed(const ContractAddress &account, GasAmount gas_limit);

  // Maps an account to the txs it has submitted.
  // TODO(jhumphri): Is [account_to_tx_] mostly only read rather than written
  // (i.e., are we inserting new accounts into it rarely)? Would it be better to
  // make it an [std::unordered_map<>] and protect it with a rw lock?
  SynchronizedMap<ContractAddress, std::map<TxKey, TxValue *>> account_to_tx_;
  std::set<AccountKey> ready_accounts_;

  // Tx cache.
  // TODO(jhumphri): We currently do not free txs from [txs_]. I think we can
  // free them automatically without too much overhead/garbage collection with
  // smart pointers (e.g., [std::shared_ptr<>]).
  SynchronizedMap<SignedTransactionHash, TxValue> txs_;
  // Account cache.
  // TODO(jhumphri): Similarly, is this read heavy? Would it be better to make
  // it an [std::unordered_map] and protect it with a rw lock?
  SynchronizedMap<ContractAddress, HogAge> accounts_;

  // The number of txs on the runqueue.
  std::atomic<size_t> size_ = 0;
  BlockHeight current_block_ = 0;

  // The RPC client that the scheduler uses to talk to the database. The
  // scheduler uses this to read/write the number of hogs and hog ages for
  // accounts.
  std::shared_ptr<db_sched_client_t> db_sched_client_;
};

} // namespace hogd
