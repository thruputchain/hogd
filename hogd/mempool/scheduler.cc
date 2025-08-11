#include "scheduler.h"

#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"

namespace hogd {
namespace {

// When doing division, we introduce imprecision into our calculations. Thus, we
// always round the result up to avoid benefitting the account. This helps to
// ensure that users are not incentivized to waste network storage or compute,
// such as by spreading HOGs among multiple accounts.
int128_t
div_round_up(int128_t numerator, int128_t denominator)
{
  return (numerator / denominator) + ((numerator % denominator) ? 1 : 0);
}

} // namespace

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
  {
    // See comment above for [div_round_up()].
    int128_t numerator = (to_schedule_hogs * to_schedule_age) +
                         (already_scheduled_hogs * already_scheduled_age);
    int128_t denominator = to_schedule_hogs + already_scheduled_hogs;
    new_averages.average_ages[detail::AverageHogAge::kGroupToSchedule] =
        div_round_up(numerator, denominator);
  }

  // Update [kGroupAlreadyScheduled].
  new_averages.num_hogs[detail::AverageHogAge::kGroupAlreadyScheduled] =
      received_hogs;
  new_averages.average_ages[detail::AverageHogAge::kGroupAlreadyScheduled] =
      received_age;

  // Update [kGroupReceived].
  new_averages.num_hogs[detail::AverageHogAge::kGroupReceived] = 0;
  new_averages.average_ages[detail::AverageHogAge::kGroupReceived] = 0;

  return write_address_average_hog_age(db_, Address({}), new_averages)
      .has_value();
}

bool
Scheduler::process_received_hogs(const AccountID &account_id,
                                 int128_t num_received,
                                 BlockHeight current_block)
{
  HogDBStatus<detail::AverageHogAge> ret =
      read_address_average_hog_age(db_, Address({}));
  if (!ret.has_value()) {
    return false;
  }
  const detail::AverageHogAge &averages = ret.value();
  detail::AverageHogAge new_averages = averages;

  int128_t received_hogs =
      averages.num_hogs[detail::AverageHogAge::kGroupReceived];
  BlockHeight received_age =
      averages.average_ages[detail::AverageHogAge::kGroupReceived];

  new_averages.num_hogs[detail::AverageHogAge::kGroupReceived] =
      received_hogs + num_received;
  {
    // See comment above for [div_round_up()].
    int128_t numerator =
        (received_hogs * received_age) + (num_received * current_block);
    int128_t denominator = received_hogs + num_received;
    new_averages.average_ages[detail::AverageHogAge::kGroupReceived] =
        div_round_up(numerator, denominator);
  }

  return write_address_average_hog_age(db_, Address({}), new_averages)
      .has_value();
}

x23::Vec<SignedTransaction>
Scheduler::schedule(size_t block_size)
{
  x23::Vec<SignedTransaction> ret;
  size_t num_scheduled = 0;
  while (num_scheduled < block_size && !txs_.empty()) {
    const size_t num_remaining = block_size - num_scheduled;

    auto &[average_age, txs] = *txs_.begin();
    if (txs.size() <= num_remaining) {
      ret.append(txs.begin(), txs.end());
      CHECK_EQ(txs_.erase(average_age), 1);
      num_scheduled += txs.size();
    }
    else {
      ret.append(txs.begin(), txs.begin() + num_remaining);
      txs.erase(txs.begin(), txs.begin() + num_remaining);
      num_scheduled += num_remaining;
    }
  }
  return ret;
}

} // namespace hogd
