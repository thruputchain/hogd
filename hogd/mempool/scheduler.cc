#include "scheduler.h"

#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"

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
      read_address_average_hog_age(db_, Address({}));
  if (!ret.has_value()) {
    return false;
  }
  const detail::AverageHogAge &averages = ret.value();
  // [add_vetted_tx] should only be called when this condition is true.
  CHECK_LE(tx.tx.invocation.gas_limit,
           averages.num_hogs[detail::AverageHogAge::kGroupToSchedule]);
  BlockHeight age =
      averages.average_ages[detail::AverageHogAge::kGroupToSchedule];
  txs_[age].push_back(tx);

  return true;
}

bool
Scheduler::update_groups(const AccountID &account_id)
{
  HogDBStatus<detail::AverageHogAge> ret =
      read_address_average_hog_age(db_, Address({}));
  if (!ret.has_value()) {
    return false;
  }
  const detail::AverageHogAge &averages = ret.value();
  detail::AverageHogAge new_averages;

  int128_t to_schedule_hogs =
      averages.num_hogs[detail::AverageHogAge::kGroupToSchedule];
  BlockHeight to_schedule_age =
      averages.average_ages[detail::AverageHogAge::kGroupToSchedule];

  int128_t already_scheduled_hogs =
      averages.num_hogs[detail::AverageHogAge::kGroupAlreadyScheduled];
  BlockHeight already_scheduled_age =
      averages.average_ages[detail::AverageHogAge::kGroupAlreadyScheduled];

  int128_t received_hogs =
      averages.num_hogs[detail::AverageHogAge::kGroupReceived];
  BlockHeight received_age =
      averages.average_ages[detail::AverageHogAge::kGroupReceived];

  // Update [kGroupToSchedule].
  new_averages.num_hogs[detail::AverageHogAge::kGroupToSchedule] =
      to_schedule_hogs + already_scheduled_hogs;
  // TODO(jhumphri): We need to deal with the lack of precision somehow.
  // Otherwise, errors will be carried forward.
  new_averages.average_ages[detail::AverageHogAge::kGroupToSchedule] =
      ((to_schedule_hogs * to_schedule_age) +
       (already_scheduled_hogs * already_scheduled_age)) /
      (to_schedule_hogs + already_scheduled_hogs);

  // Update [kGroupAlreadyScheduled].
  new_averages.num_hogs[detail::AverageHogAge::kGroupAlreadyScheduled] =
      received_hogs;
  new_averages.average_ages[detail::AverageHogAge::kGroupAlreadyScheduled] =
      received_age;

  // Update [kGroupReceived].
  new_averages.num_hogs[detail::AverageHogAge::kGroupReceived] = 0;
  new_averages.average_ages[detail::AverageHogAge::kGroupReceived] = 0;

  return true;
}

} // namespace hogd
