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
                             int128_xdr num_received,
                             BlockHeight current_block);
  std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
  schedule(size_t block_size);

  bool read_average_hog_age(const ContractAddressBatch &addresses,
                            HogAgeBatch &ages);
  bool write_average_hog_age(const HogAgeBatch &ages);

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

      auto it = tx_hashes_.find(hash);
      if (it == tx_hashes_.end()) {
        return false;
      }
      ret.push_back({.signed_tx = it->second.tx,
                     .nondeterministic_proposal_data =
                         nominated_tx.nondeterministic_proposal_data});

      // We access [it] below, so we call [txs_.erase()] before
      // [tx_hashes_.erase()] to avoid accessing an invalidated iterator.
      CHECK_EQ(txs_[it->second.age].erase(&it->second), 1);
      tx_hashes_.erase(it);
    }
    return true;
  }

private:
  struct TxInfo {
    BlockHeight age;
    SignedTransactionHash tx_hash;
    SignedTransaction tx;
  };

  void add_txs_from_info(std::unordered_set<TxInfo *> &info, size_t num,
                         x23::Vec<SignedTransactionHash> &hashes,
                         x23::Vec<SignedTransaction> &txs);

  db_client_t &db_client_;
  std::unordered_map<SignedTransactionHash, TxInfo> tx_hashes_;
  std::map<BlockHeight, std::unordered_set<TxInfo *>> txs_;
};

} // namespace hogd
