#pragma once

#include <map>
#include <unordered_set>

#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"
#include "types.h"

namespace hogd {

class Scheduler {
public:
  Scheduler(db_client_t &db_client) : db_client_(db_client) {}

  bool add_vetted_tx(const SignedTransaction &tx);
  bool update_groups(const ContractAddress &account_id);
  bool process_received_hogs(const ContractAddress &account_id,
                             int128_xdr num_received,
                             BlockHeight current_block);
  x23::Vec<SignedTransaction> schedule(size_t block_size);

  bool read_average_hog_age(const ContractAddressBatch &addresses,
                            HogAgeBatch &ages);
  bool write_average_hog_age(const HogAgeBatch &ages);

private:
  db_client_t &db_client_;

  std::unordered_set<SHA256Hash> tx_hashes_;
  std::map<BlockHeight, x23::Vec<SignedTransaction>> txs_;
};

} // namespace hogd
