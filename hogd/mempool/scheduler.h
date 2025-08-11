#pragma once

#include <map>
#include <unordered_set>

#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"

namespace hogd {

class Scheduler {
public:
  Scheduler(HogDatabase *db) : db_(db) {}

  bool add_vetted_tx(const SignedTransaction &tx);
  bool update_groups(const AccountID &account_id);
  bool process_received_hogs(const AccountID &account_id, int128_t num_received,
                             BlockHeight current_block);
  x23::Vec<SignedTransaction> schedule(size_t block_size);

private:
  HogDatabase *db_;

  std::unordered_set<SHA256Hash> tx_hashes_;
  std::map<BlockHeight, x23::Vec<SignedTransaction>> txs_;
};

} // namespace hogd
