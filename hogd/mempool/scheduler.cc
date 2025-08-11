#include "scheduler.h"

namespace hogd {

bool
Scheduler::add_vetted_tx(const AccountID &account_id,
                         const SignedTransaction &tx)
{
  if (!tx_hashes_.insert(sha256(tx)).second) {
    // The transaction is already present in the runqueue.
    return true;
  }

  HogDBStatus<detail::AverageHogAge> ret =
      read_address_average_hog_age(db_, {});
  if (!ret.has_value()) {
    return false;
  }
  const detail::AverageHogAge &averages = ret.value();
  // [add_vetted_tx] should only be called when this condition is true.
  CHECK_LE(tx.invocation.gas_limit,
           averages.num_hogs[detail::AverageHogAge::kGroupToSchedule]);
  BlockHeight age =
      averages.average_ages[detail::AverageHogAge::kGroupToSchedule];
  txs_.[age].push_back(tx);

  return true;
}

bool Scheduler::update_groups(const AccountID &account_id) {
  return true;
}

} // namespace hogd
