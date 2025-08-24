#pragma once

#include <map>
#include <unordered_set>

#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/hex_array.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"
#include "types.h"

namespace hogd {

class Scheduler {
public:
  Scheduler(db_client_t &db_client) : db_client_(db_client) {}

  // Adds txs to the scheduler. Adding the same tx multiple times is okay; the
  // scheduler detects and silently drops the duplicates rather than enqueue
  // them.
  void add_vetted_txs(const SignedTransactionBatch &txs);

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
  void cycle_groups(const ContractAddress &account_address);

  // Call this method when account [account_address] receives [num_received]
  // hogs from another account in block [current_block]. This method adds the
  // hogs to the [kHogAgeGroupReceived] group.
  void process_received_hogs(const ContractAddress &account_address,
                             uint128_xdr num_received,
                             BlockHeight current_block);

  // TODO(jhumphri): I should rework the code to expose [update_accounts_helper]
  // rather than [update_accounts]. The latter function should not exist as I
  // cannot think of a situation where we would want to blindly overwrite the
  // cache with the contents of the database.
  // In the tests, we should instead use [get_cached_hog_age] to change hog
  // amounts and then call [update_accounts_helper].
  void update_accounts(const x23::Vec<ContractAddress> &accounts);

  std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
  schedule(size_t block_size);

  // Call this method when txs are successfully executed *as part of a block
  // that this node proposed*. This method just removes the remaining tx state
  // from the scheduler, so we need to call this to prevent a memory leak.
  void mark_txs_executed(const x23::Vec<SignedTransactionHash> &hashes);

  // Call this method when txs failed to execute *as part of a block that this
  // node proposed*. This method adds the hogs consumed by the tx back to the
  // account and removes the remaining tx state from the scheduler. The txs are
  // then returned by the method.
  //
  // Note: I initially thought it made sense to have this method add the txs
  // back to the runqueue. However, txs may have failed due to irrecoverable
  // errors, such as sending too many hogs to another account. Thus, this method
  // returns the failed txs and the caller can choose to what to do next,
  // whether that is reenqueing the txs in the scheduler, dropping the txs, or
  // doing something else.
  SignedTransactionBatch
  mark_txs_failed(const x23::Vec<SignedTransactionHash> &hashes);

  // Finds the txs specified in [nomination] and removes their state from the
  // scheduler. This is used when validating a block as the txs have supposedly
  // been committed to a block already, so we do not want to schedule the txs.
  // But the validator still needs to get the tx state because it needs to
  // re-execute the txs to verify the block.
  //
  // This method fills in [ret] with [ProposedTransactions] to verify. This
  // method included the nondeterministic proposal data for each tx in [ret].
  //
  // TODO(jhumphri): I do not like this method signature. The parameter types
  // are too tied to the mempool rather than the scheduler.
  bool find_and_remove_vetted_txs(const Nomination &nomination,
                                  x23::Vec<ProposedTransaction> &ret);

  // We are exposing the functions below for testing. They should not be called
  // directly by non-test code.
  bool read_average_hog_age(const ContractAddressBatch &addresses, bool force);
  HogAge &get_cached_hog_age(const ContractAddress &address);
  bool write_average_hog_age(const ContractAddressBatch &addresses);

private:
  struct TxInfo {
    BlockHeight age;
    ContractAddress account;
    SignedTransactionHash tx_hash;
    SignedTransaction tx;
  };

  void add_txs_from_info(std::unordered_set<TxInfo *> &info, size_t num,
                         x23::Vec<SignedTransactionHash> &hashes,
                         x23::Vec<SignedTransaction> &txs);

  db_client_t &db_client_;
  std::unordered_map<SignedTransactionHash, TxInfo> hash_to_tx_;
  std::unordered_map<ContractAddress, std::unordered_set<TxInfo *>>
      account_to_tx_;
  std::map<BlockHeight, std::unordered_set<TxInfo *>> age_to_tx_;

  std::unordered_map<ContractAddress, HogAge> cached_hogs_;
  std::unordered_map<ContractAddress, std::unordered_set<TxInfo *>> stranded_;
};

} // namespace hogd
