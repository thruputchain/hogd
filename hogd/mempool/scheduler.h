#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/synchronized_map.h"
#include "hogd/mempool/shared.h"
#include "sched_db.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/x23/async.h"
#include "x23/x23/corosync.h"
#include "x23/x23/vec.h"

namespace hogd {

class Scheduler {
public:
  Scheduler(BlockHeight current_block,
            std::shared_ptr<db_sched_client_t> db_sched_client)
    : current_block_(current_block),
      db_sched_client_(std::move(db_sched_client))
  {}

  ~Scheduler() { wait_for_pending_detached_coroutines(); }

  x23::Async<void> shut_down()
  {
    wait_for_pending_detached_coroutines();
    co_return;
  }

  // Adds txs to the scheduler. Adding the same tx multiple times is okay; the
  // scheduler detects and silently drops the duplicates rather than enqueue
  // them.
  //
  // This method will cycle the groups for an account if doing so gives the
  // account an advantage in scheduling. This happens when (1) cycling increases
  // the average age of the [kHogAgeGroupToSchedule] group or (2) the
  // [kHogAgeGroupToSchedule] group does not have enough hogs to schedule the
  // tx but cycling will add enough hogs to this group to schedule it.
  x23::Async<void> add_txs(const x23::XdrVec<SignedTransaction> &txs);

  x23::Async<void> process_transfers(const HogsTransferred &transfer);

  // Chooses txs to schedule in the next block. [block_height] is the height of
  // the block that is being scheduled. [block_size] is the maximum number of
  // txs that will be included in the next block, though fewer txs than
  // [block_size] may be included.
  void schedule(size_t num_txs, x23::XdrVec<SignedTransactionHash> &tx_hashes,
                x23::XdrVec<SignedTransaction> &txs);

  void start_pending_nomination(BlockID block_id);
  void commit_pending_nomination(BlockID block_id);
  void abandon_pending_nomination(BlockID block_id);
  x23::Async<void>
  apply_committed_block_hog_ages(const x23::XdrVec<SignedTransaction> &txs,
                                 BlockHeight block_height);

  std::shared_ptr<const SignedTransaction>
  find_tx(const SignedTransactionHash &tx_hash)
  {
    auto [tx_value_ptr, _] = txs_.find(tx_hash);
    if (tx_value_ptr) {
      return std::shared_ptr<const SignedTransaction>(*tx_value_ptr,
                                                      &(*tx_value_ptr)->tx);
    }
    return {};
  }

  void get_txs_for_account(const ContractAddress &address,
                           SignedTransactionHashBatch &tx_hashes,
                           SignedTransactionBatch &txs)
  {
    // The method is called by [MempoolCoordinatorRpc], which may try to find
    // txs associated with an account that the scheduler does not know about. In
    // that case, just return.
    auto [account_state, lock] = accounts_.find(address);
    if (!account_state) {
      return;
    }

    tx_hashes.reserve(account_state->txs.size());
    txs.reserve(account_state->txs.size());
    for (const auto &[tx_key, tx_value] : account_state->txs) {
      tx_hashes.push_back(tx_key.tx_hash);
      txs.push_back(tx_value->tx);
    }
  }

  void reenqueue_txs(const auto &begin, const auto &end)
  {
    size_t num_inserted = 0;
    for (auto it = begin; it != end; it++) {
      const SignedTransactionHash &tx_hash = *it;
      const auto [tx_value_ptr, lock] = txs_.find(tx_hash);
      CHECK_PTR(tx_value_ptr);
      if (insert_tx_into_runqueue(tx_hash, *tx_value_ptr)) {
        num_inserted++;
      }
    }
    size_.fetch_add(num_inserted, std::memory_order_release);
  }

  x23::Async<void>
  remove_from_scheduler(x23::span<const SignedTransactionHash> hashes);

  // We are exposing the functions below for testing. They should not be called
  // directly by non-test code.

  std::pair<HogAge *, std::unique_lock<std::mutex>>
  get_cached_hog_age(const ContractAddress &address);

  void write_all_average_hog_ages_to_db();

  struct ReadAverageHogAgesResult {
    std::unordered_set<ContractAddress, ByteArrayHasher> found;
    std::unordered_set<ContractAddress, ByteArrayHasher> missing;
  };
  x23::Async<ReadAverageHogAgesResult>
  read_average_hog_ages(const x23::XdrVec<ContractAddress> &accounts);
  x23::Async<ReadAverageHogAgesResult>
  read_average_hog_ages(const x23::XdrVec<SignedTransaction> &txs);
  x23::Async<ReadAverageHogAgesResult>
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

  BlockHeight current_block() const
  {
    return current_block_.load(std::memory_order_relaxed);
  }
  void increment_current_block()
  {
    current_block_.fetch_add(1, std::memory_order_relaxed);
  }
  void set_current_block(BlockHeight height)
  {
    current_block_.store(height, std::memory_order_relaxed);
  }

  // The number of txs in the scheduler, including both those of the runqueue
  // and those that are stranded.
  size_t size() const { return size_.load(std::memory_order_relaxed); }
  size_t tx_cache_size() { return txs_.size(); }

  // Debug.
  void dump_runqueue();

  x23::CoroFutex<int64_t> transfer_finished_{-1};
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
      // [std::array] implements lexicographic comparison.
      return tx_hash < other.tx_hash;
    }
  };

  struct TxValue {
    // The tx itself.
    SignedTransaction tx;
  };

  struct AccountState {
    HogAge hog_age{};
    std::map<TxKey, std::shared_ptr<TxValue>> txs;
  };

  struct PendingNominationState {
    BlockID block_id = 0;
    std::unordered_map<ContractAddress, HogAge, ByteArrayHasher> hog_ages;
    std::unordered_set<ContractAddress, ByteArrayHasher> touched_accounts;

    PendingNominationState() { clear(); }

    void clear()
    {
      block_id = 0;
      hog_ages.clear();
      touched_accounts.clear();
    }
  };

  struct AccountKey {
    BlockHeight average_hog_age;
    ContractAddress account;

    bool operator<(const AccountKey &other) const noexcept
    {
      if (average_hog_age != other.average_hog_age) {
        return average_hog_age < other.average_hog_age;
      }
      // [std::array] implements lexicographic comparison.
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
                               const std::shared_ptr<TxValue> &tx_value);
  bool add_txs_helper(const SignedTransaction &tx);

  // TODO(jhumphri): Give this a better name.
  void cycle_accounts_during_schedule_helper(
      const std::unordered_set<ContractAddress, ByteArrayHasher> &accounts,
      const TxKey &lower_bound);

  void process_transfers_helper(const Delta &delta,
                                BlockHeight block_height) noexcept;
  static bool apply_committed_tx_hog_age(const SignedTransaction &tx,
                                         HogAge &hog_age,
                                         uint128_t &hogs_spent_by_account,
                                         BlockHeight block_height);

  static void move_hogs_to_already_scheduled_group(HogAge &hog_ages,
                                                   uint128_t num_hogs,
                                                   BlockHeight block_height);

  x23::Detached write_all_average_hog_ages_helper();

  std::pair<AccountState *, std::unique_lock<std::mutex>>
  get_account_state(const ContractAddress &address);

  void wait_for_pending_detached_coroutines()
  {
    while (pending_detached_coroutines_.load(std::memory_order_acquire) > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // This method will cycle the groups for [account] if doing so gives the
  // account an advantage in scheduling. This happens when (1) cycling increases
  // the average age of the [kHogAgeGroupToSchedule] group or (2) no txs are
  // currently on the runqueue for [account] and the [kHogAgeGroupToSchedule]
  // group does not have enough hogs to schedule a tx with a gas limit of
  // [gas_limit].
  void cycle_if_needed(const ContractAddress &account, GasAmount gas_limit);
  static bool cycle_if_needed(HogAge &hog_age, GasAmount gas_limit);
  HogAge *get_hog_age_from_pending_nomination_create_if_needed(
      AccountState &account_state, const ContractAddress &account);
  const HogAge &get_hog_age_from_pending_nomination_if_exists(
      const AccountState &account_state, const ContractAddress &account) const;
  void refresh_ready_accounts(
      const std::unordered_set<ContractAddress, ByteArrayHasher> &accounts);
  void erase_ready_account(const ContractAddress &account);

  std::set<AccountKey> ready_accounts_;
  PendingNominationState pending_nomination_;

  // Tx cache.
  // TODO(jhumphri): We currently do not free txs from [txs_]. I think we can
  // free them automatically without too much overhead/garbage collection with
  // smart pointers (e.g., [std::shared_ptr<>]).
  SynchronizedMap<SignedTransactionHash, std::shared_ptr<TxValue>,
                  ByteArrayHasher>
      txs_;
  // Per-account scheduler state, including hog ages and the account runqueue.
  SynchronizedMap<ContractAddress, AccountState, ByteArrayHasher> accounts_;

  // The number of txs on the runqueue.
  std::atomic<size_t> size_ = 0;
  std::atomic<BlockHeight> current_block_ = -1;

  // The RPC client that the scheduler uses to talk to the database. The
  // scheduler uses this to read/write the number of hogs and hog ages for
  // accounts.
  std::shared_ptr<db_sched_client_t> db_sched_client_;

  std::atomic<ssize_t> pending_detached_coroutines_ = 0;
};

} // namespace hogd
