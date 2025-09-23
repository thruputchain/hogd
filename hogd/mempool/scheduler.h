#pragma once

#include <iostream>
#include <map>
#include <unordered_set>

#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/hex_array.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "types.h"

namespace hogd {

class Scheduler {
public:
  Scheduler(db_sched_client_t &db_sched_client)
    : db_sched_client_(db_sched_client)
  {}

  // Destructors cannot throw exceptions, though the destructor below calls
  // [write_cache()], which is capable of throwing an exception (due to the
  // [CHECK()]). We need to catch and squash this exception in the destructor so
  // that the destructor itself cannot throw an exception.
  //
  // We know that the CHECK will never fail. Thus, in the event of an exception,
  // we just print an error message to [stderr] and take no further action.
  ~Scheduler()
  {
    try {
      write_cache();
    } catch (const std::exception &e) {
      std::cerr << "[Scheduler::~Scheduler] Exception in write_cache(): "
                << e.what() << '\n';
    } catch (...) {
      std::cerr << "[Scheduler::~Scheduler] Unknown exception in write_cache()"
                << '\n';
    }
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
  void add_txs(const SignedTransactionBatch &txs);

  // Call this method when account [account_address] receives [num_received]
  // hogs from another account in block [current_block]. This method adds the
  // hogs to the [kHogAgeGroupReceived] group.
  void process_received_hogs(const ContractAddress &account,
                             uint128_xdr num_received,
                             BlockHeight current_block);

  // Reorders txs in the runqueue and adds/removes txs from the runqueue based
  // on the number of hogs and hog ages for an account. Note that this function
  // assumes [cached_hogs_[account]] contains this information for [account], so
  // before calling this function, [cached_hogs_[account]] must be updated.
  void update_accounts(const x23::Vec<ContractAddress> &accounts);

  // Chooses txs to schedule in the next block. [block_height] is the height of
  // the block that is being scheduled. [block_size] is the maximum number of
  // txs that will be included in the next block, though fewer txs than
  // [block_size] may be included.
  std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
  schedule(BlockHeight block_height, size_t block_size);

  // Call this method when txs are successfully executed *as part of a block
  // that this node proposed*. This method just removes the remaining tx state
  // from the scheduler, so we need to call this to prevent a memory leak.
  void mark_txs_executed(const x23::Vec<SignedTransactionHash> &hashes,
                         BlockHeight block);

  // Call this method when txs failed to execute. This method adds the hogs
  // consumed by the tx back to the account and removes the remaining tx state
  // from the scheduler. The txs are then returned by the method.
  //
  // Note: I initially thought it made sense to have this method add the txs
  // back to the runqueue. However, txs may have failed due to irrecoverable
  // errors, such as sending too many hogs to another account. Thus, this method
  // returns the failed txs and the caller can choose to what to do next,
  // whether that is reenqueing the txs in the scheduler, dropping the txs, or
  // doing something else.
  SignedTransactionBatch
  mark_txs_failed(const x23::Vec<SignedTransactionHash> &hashes);

  // Finds the txs specified in [hashes] and removes them from the runqueue.
  // This is used when validating a block as the txs have supposedly been
  // committed to a block already, so we do not want to schedule the txs. But
  // the validator still needs to get the tx state because it needs to
  // re-execute the txs to verify the block.
  //
  // This method fills in [ret] with the [SignedTransaction]s to verify.
  //
  // Note that this method removes the txs from the runqueue, but it does not
  // remove their state entirely from the scheduler. Specifically, information
  // about the txs is still held in [hash_to_tx_]. The caller needs to
  // ultimately call [mark_txs_executed()] to remove this state.
  // TODO(jhumphri): We should pass a set instead of a vector, both here and in
  // [mark_txs_executed()] and the other functions. Otherwise, a malicious
  // nomination could crash a node by including the same hash twice and
  // triggering a CHECK failure.
  bool
  find_and_remove_from_runqueue(const x23::Vec<SignedTransactionHash> &hashes,
                                x23::Vec<SignedTransaction> *ret);

  // We are exposing the functions below for testing. They should not be called
  // directly by non-test code.

  bool read_average_hog_age(const ContractAddressBatch &addresses, bool force);
  HogAge &get_cached_hog_age(const ContractAddress &address);
  bool write_average_hog_age(const ContractAddressBatch &addresses);

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
  void cycle_groups(const ContractAddress &account);

  void filter_txs_for_account(
      const ContractAddress &account,
      const std::function<bool(const SignedTransaction &tx)> &filter);

  // Debug.
  void dump_runqueue() const;

private:
  // We use this type to store information about a tx in the runqueue.
  struct TxInfo {
    // The average age of hogs in the account that submitted this tx. This age
    // dictates the scheduling priority of the tx.
    BlockHeight age;
    // The account that submitted the tx.
    ContractAddress account;
    // The hash of the tx.
    SignedTransactionHash tx_hash;
    // The tx itself.
    SignedTransaction tx;
  };

  // Writes the contents of [cached_hogs_] back to the database.
  //
  // Note that the scheduler removes hogs from the [kHogAgeGroupToSchedule]
  // group when a tx is added to the runqueue, *not* when the tx executes.
  // However, we want the [kHogAgeGroupToSchedule] group we write to hogdb to
  // contain these hogs. Thus, this method adds the deducted hogs back to the
  // [kHogAgeGroupToSchedule] group for each account in [cached_hogs_], then
  // writes [cached_hogs_] to hogdb, and then deducts the hogs again from
  // [cached_hogs_]. So [cached_hogs_] is the same both before and after this
  // method calls, and the [kHogAgeGroupToSchedule] group written to hogdb
  // contains the correct number of hogs.
  void write_cache();

  // This method will cycle the groups for [account] if doing so gives the
  // account an advantage in scheduling. This happens when (1) cycling increases
  // the average age of the [kHogAgeGroupToSchedule] group or (2) no txs are
  // currently on the runqueue for [account] and the [kHogAgeGroupToSchedule]
  // group does not have enough hogs to schedule a tx with a gas limit of
  // [gas_limit].
  void cycle_if_needed(const ContractAddress &account, GasAmount gas_limit);

  void remove_tx_from_runqueue(TxInfo *info,
                               x23::Vec<SignedTransactionHash> &hashes,
                               x23::Vec<SignedTransaction> &txs);

  // Helper function for [schedule()]. Removes txs from the runqueue. Note that
  // removing txs from the runqueue is a bit cumbersome because we maintain
  // several data structures to make it easy to look up txs with various key
  // types.
  void get_txs_all(std::unordered_set<TxInfo *> &info,
                   x23::Vec<SignedTransactionHash> &hashes,
                   x23::Vec<SignedTransaction> &txs);

  void get_txs_subset(std::unordered_set<TxInfo *> &info, size_t num,
                      x23::Vec<SignedTransactionHash> &hashes,
                      x23::Vec<SignedTransaction> &txs);

  // Discards all txs that have expired before [current_height]. In other words,
  // if [current_height] is the height of the current block being scheduled,
  // then all txs that expire at heights [current_height - 1] and smaller are
  // discarded.
  void discard_expired_txs(BlockHeight current_height);
  void discard_txs(const x23::Vec<SignedTransactionHash> &tx_hashes);

  bool on_runqueue(TxInfo &info)
  {
    return account_to_tx_.contains(info.account) &&
           account_to_tx_[info.account].contains(&info);
  }

  // The RPC client that the scheduler uses to talk to the database. The
  // scheduler uses this to read/write the number of hogs and hog ages for
  // accounts.
  db_sched_client_t &db_sched_client_;

  // Caches the number of hogs and hog ages for an account so that the scheduler
  // does not need to make as many RPC calls to the database.
  std::unordered_map<ContractAddress, HogAge> cached_hogs_;

  // Maps a tx hash to information about the tx.
  std::unordered_map<SignedTransactionHash, TxInfo> hash_to_tx_;

  // ---------------
  // Runqueue data structures. We maintain several data structures to make it
  // easy to order the runqueue and look up txs with several key types.
  //
  // These data structures *only* hold txs that are currently on the runqueue,
  // i.e., eligible to be scheduled! Txs that are not eligible to be scheduled,
  // such as due to an insuffucient hog balance on the submitting account, are
  // stored in [stranded_] below.

  // Maps an account to the txs it has submitted.
  std::unordered_map<ContractAddress, std::unordered_set<TxInfo *>>
      account_to_tx_;
  // Maps an average hog age to a set of txs submitted by accounts with that
  // average hog age. The scheduler uses this map to find the txs with the
  // largest hog age as those txs are scheduled with the highest priority.
  std::map<BlockHeight, std::unordered_set<TxInfo *>> age_to_tx_;

  // ---------------
  // Stranded txs.

  // Holds all txs that cannot be scheduled, typically because an account does
  // not have enough hogs. Maps an account to its stranded txs. This makes it
  // easy to look up which stranded txs correspond to an account, since we will
  // want to know this once the number of hogs owned by an account changes.
  std::unordered_map<ContractAddress,
                     std::map<GasAmount, std::unordered_set<TxInfo *>>>
      stranded_;

  // ---------------
  // Other data structures for txs.

  // Maps a block expiry height to a set of txs that expire at that height. So
  // in other words, if a set of txs expire at block height 10, those txs can be
  // included in block 10 and all blocks with smaller heights, but they cannot
  // be included in blocks 11+.
  //
  // This data structure holds *all* txs. So in other words, it holds all txs on
  // the runqueue and all txs in [stranded_]. When we advance past a block
  // height, we want to discard all txs that expire at that block height,
  // regardless of whether they are on the runqueue or stranded.
  std::map<BlockHeight, std::unordered_set<TxInfo *>> expiry_to_tx_;
};

} // namespace hogd
