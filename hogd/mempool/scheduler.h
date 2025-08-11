#pragma once

#include <map>
#include <unordered_set>

#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"

namespace hogd {

class Scheduler {
public:
  Scheduler(HogDatabase &db) : db_(db) {}

  bool add_vetted_tx(const AccountID &account_id, const SignedTransaction &tx);
  bool update_groups(const AccountID &account_id);
  x23::Vec<SignedTransaction> schedule();

private:
  HogDatabase &db_;

  std::unordered_set<SHA256Hash> tx_hashes_;
  std::map<BlockHeight, std::vector<SignedTransaction>> txs_;
};

} // namespace hogd
