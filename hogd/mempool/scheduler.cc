#include "scheduler.h"

#include "db_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "nomination.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {
namespace {

// When doing division, we introduce imprecision into our calculations. Thus, we
// always round the result up to avoid benefitting the account. This helps to
// ensure that users are not incentivized to waste network storage or compute,
// such as by spreading HOGs among multiple accounts.
int128_xdr
div_round_up(int128_xdr numerator, int128_xdr denominator)
{
  int128_t n = from_xdr(numerator);
  int128_t d = from_xdr(denominator);
  int128_t result = (n / d) + ((n % d) ? 1 : 0);
  return to_xdr(result);
}

} // namespace

bool
Scheduler::read_average_hog_age(const ContractAddressBatch &addresses,
                                HogAgeBatch &ages)
{
  HogAgeResult res = db_client_.dbcoord_get_hog_ages(addresses)();
  if (res.result() != DbResult::DbResultOK) {
    return false;
  }
  ages = res.hog_age_batch();
  return true;
}

bool
Scheduler::write_average_hog_age(const HogAgeBatch &ages)
{
  return db_client_.dbcoord_write_hog_ages(ages)() == DbResult::DbResultOK;
}

bool
Scheduler::add_vetted_txs(const SignedTransactionBatch &txs)
{
  // TODO(jhumphri): Optimize this with coroutines.
  for (const SignedTransaction &tx : txs) {
    SignedTransactionHash tx_hash = sha256(tx);
    if (tx_hashes_.count(tx_hash)) {
      // The transaction is already present in the runqueue.
      return true;
    }

    HogAgeBatch ages;
    if (!read_average_hog_age({tx.tx.invocation.invoked_address}, ages)) {
      return false;
    }
    const HogAge &averages = ages[0];
    // [add_vetted_tx] should only be called when this condition is true.
    CHECK_LE(tx.tx.invocation.gas_limit,
             from_xdr(averages.num_hogs[kHogAgeGroupToSchedule]));
    BlockHeight age = averages.average_ages[kHogAgeGroupToSchedule];

    TxInfo info{.age = age, .tx_hash = tx_hash, .tx = tx};
    CHECK(tx_hashes_.insert({tx_hash, info}).second);
    txs_[age].push_back(info);
  }

  return true;
}

bool
Scheduler::update_groups(const ContractAddress &account_address)
{
  HogAgeBatch ages;
  if (!read_average_hog_age({account_address}, ages)) {
    return false;
  }
  const HogAge &averages = ages[0];
  HogAge new_averages;
  new_averages.address = averages.address;

  const int128_xdr to_schedule_hogs = averages.num_hogs[kHogAgeGroupToSchedule];
  const BlockHeight to_schedule_age =
      averages.average_ages[kHogAgeGroupToSchedule];

  const int128_xdr already_scheduled_hogs =
      averages.num_hogs[kHogAgeGroupAlreadyScheduled];
  const BlockHeight already_scheduled_age =
      averages.average_ages[kHogAgeGroupAlreadyScheduled];

  const int128_xdr received_hogs = averages.num_hogs[kHogAgeGroupReceived];
  const BlockHeight received_age = averages.average_ages[kHogAgeGroupReceived];

  // Update [kGroupToSchedule].
  new_averages.num_hogs[kHogAgeGroupToSchedule] =
      to_xdr(from_xdr(to_schedule_hogs) + from_xdr(already_scheduled_hogs));
  {
    // See comment above for [div_round_up()].
    int128_xdr numerator =
        to_xdr((from_xdr(to_schedule_hogs) * to_schedule_age) +
               (from_xdr(already_scheduled_hogs) * already_scheduled_age));
    int128_xdr denominator =
        to_xdr(from_xdr(to_schedule_hogs) + from_xdr(already_scheduled_hogs));
    new_averages.average_ages[kHogAgeGroupToSchedule] =
        from_xdr(div_round_up(numerator, denominator));
  }

  // Update [kGroupAlreadyScheduled].
  new_averages.num_hogs[kHogAgeGroupAlreadyScheduled] = received_hogs;
  new_averages.average_ages[kHogAgeGroupAlreadyScheduled] = received_age;

  // Update [kGroupReceived].
  new_averages.num_hogs[kHogAgeGroupReceived] =
      to_xdr(static_cast<int128_t>(0));
  new_averages.average_ages[kHogAgeGroupReceived] = 0;

  return write_average_hog_age({new_averages});
}

bool
Scheduler::process_received_hogs(const ContractAddress &account_address,
                                 int128_xdr num_received,
                                 BlockHeight current_block)
{
  HogAgeBatch ages;
  if (!read_average_hog_age({account_address}, ages)) {
    return false;
  }
  const HogAge &averages = ages[0];
  HogAge new_averages = averages;

  const int128_xdr received_hogs = averages.num_hogs[kHogAgeGroupReceived];
  const BlockHeight received_age = averages.average_ages[kHogAgeGroupReceived];

  new_averages.num_hogs[kHogAgeGroupReceived] =
      to_xdr(from_xdr(received_hogs) + from_xdr(num_received));
  {
    // See comment above for [div_round_up()].
    int128_xdr numerator = to_xdr((from_xdr(received_hogs) * received_age) +
                                  (from_xdr(num_received) * current_block));
    int128_xdr denominator =
        to_xdr(from_xdr(received_hogs) + from_xdr(num_received));
    new_averages.average_ages[kHogAgeGroupReceived] =
        from_xdr(div_round_up(numerator, denominator));
  }

  return write_average_hog_age({new_averages});
}

void
Scheduler::add_txs_from_info(const x23::Vec<TxInfo> &info, size_t num,
                             x23::Vec<SignedTransactionHash> &hashes,
                             x23::Vec<SignedTransaction> &txs)
{
  for (const TxInfo &i : info) {
    CHECK_EQ(tx_hashes_.count(i.tx_hash), 1);
    hashes.push_back(i.tx_hash);
    txs.push_back(i.tx);
    tx_hashes_.erase(i.tx_hash);
  }
}

std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
Scheduler::schedule(size_t block_size)
{
  printf("in here, size is %zu\n", txs_.size());
  x23::Vec<SignedTransactionHash> hashes;
  x23::Vec<SignedTransaction> txs;

  size_t num_scheduled = 0;
  while (num_scheduled < block_size && !txs_.empty()) {
    const size_t num_remaining = block_size - num_scheduled;

    auto &[average_age, info] = *txs_.begin();
    if (info.size() <= num_remaining) {
      // [add_txs_from_info] removes the hashes from [tx_hashes_].
      add_txs_from_info(info, info.size(), hashes, txs);
      CHECK_EQ(txs_.erase(average_age), 1);
      num_scheduled += info.size();
    }
    else {
      // [add_txs_from_info] removes the hashes from [tx_hashes_].
      add_txs_from_info(info, num_remaining, hashes, txs);
      info.erase(info.begin(), info.begin() + num_remaining);
      num_scheduled += num_remaining;
    }
  }
  return {hashes, txs};
}

} // namespace hogd
