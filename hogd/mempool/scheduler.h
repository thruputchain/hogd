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

  bool add_vetted_txs(const SignedTransactionBatch &txs);
  bool update_groups(const ContractAddress &account_address);
  bool process_received_hogs(const ContractAddress &account_address,
                             uint128_xdr num_received,
                             BlockHeight current_block);
  bool update_accounts(const x23::Vec<ContractAddress> &accounts);

  std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
  schedule(size_t block_size);

  bool read_average_hog_age(const ContractAddressBatch &addresses,
                            bool force = false);
  HogAge &get_cached_hog_age(const ContractAddress &address)
  {
    auto it = cached_hogs_.find(address);
    CHECK(it != cached_hogs_.end());
    return it->second;
  }

  bool write_average_hog_age(const ContractAddressBatch &addresses);

  void mark_txs_executed(const x23::Vec<SignedTransactionHash> &hashes)
  {
    for (const SignedTransactionHash &hash : hashes) {
      CHECK_EQ(hash_to_tx_.erase(hash), 1UL);
    }
  }

  void mark_txs_failed(const x23::Vec<SignedTransactionHash> &hashes)
  {
    for (const SignedTransactionHash &hash : hashes) {
      TxInfo &info = hash_to_tx_[hash];
      cached_hogs_[info.tx.tx.invocation.invoked_address]
          .num_hogs[kHogAgeGroupToSchedule] =
          to_xdr(from_xdr(cached_hogs_[info.tx.tx.invocation.invoked_address]
                              .num_hogs[kHogAgeGroupToSchedule]) +
                 info.tx.tx.invocation.gas_limit);
      CHECK_EQ(hash_to_tx_.erase(hash), 1UL);
    }
  }

  bool find_and_remove_vetted_txs(const Nomination &nomination,
                                  x23::Vec<ProposedTransaction> &ret)
  {
    CHECK_EQ(nomination.transaction_hashes.size(),
             nomination.transaction_nominations.size());

    ret.clear();
    for (size_t i = 0; i < nomination.transaction_hashes.size(); i++) {
      const SignedTransactionHash &hash = nomination.transaction_hashes[i];
      const NominatedTransaction &nominated_tx =
          nomination.transaction_nominations[i];

      auto it = hash_to_tx_.find(hash);
      if (it == hash_to_tx_.end()) {
        return false;
      }
      ret.push_back({.signed_tx = it->second.tx,
                     .nondeterministic_proposal_data =
                         nominated_tx.nondeterministic_proposal_data});

      // We access [it] below before calling [hash_to_tx_.erase()] to avoid
      // accessing an invalidated iterator.
      CHECK_EQ(account_to_tx_[it->second.account].erase(&it->second), 1UL);
      CHECK_EQ(age_to_tx_[it->second.age].erase(&it->second), 1UL);
      hash_to_tx_.erase(it);
    }
    return true;
  }

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
