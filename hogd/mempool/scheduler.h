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

  void add_vetted_txs(const SignedTransactionBatch &txs);
  void update_groups(const ContractAddress &account_address);
  void process_received_hogs(const ContractAddress &account_address,
                             uint128_xdr num_received,
                             BlockHeight current_block);
  void update_accounts(const x23::Vec<ContractAddress> &accounts);

  std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
  schedule(size_t block_size);

  void mark_txs_executed(const x23::Vec<SignedTransactionHash> &hashes);
  SignedTransactionBatch
  mark_txs_failed(const x23::Vec<SignedTransactionHash> &hashes);
  bool find_and_remove_vetted_txs(const Nomination &nomination,
                                  x23::Vec<ProposedTransaction> &ret);
  // We are exposing the functions below for testing. They should not be called
  // directly by non-test code.
  bool read_average_hog_age(const ContractAddressBatch &addresses,
                            bool force = false);
  HogAge &get_cached_hog_age(const ContractAddress &address)
  {
    auto it = cached_hogs_.find(address);
    CHECK(it != cached_hogs_.end());
    return it->second;
  }

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
