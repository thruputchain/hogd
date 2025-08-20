#include "scheduler.h"

#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "nomination.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include <unordered_set>
#include <utility>

namespace hogd {
namespace {

// When doing division, we introduce imprecision into our calculations. Thus, we
// always round the result up to avoid benefitting the account. This helps to
// ensure that users are not incentivized to waste network storage or compute,
// such as by spreading HOGs among multiple accounts.
uint128_xdr
div_round_up(uint128_xdr numerator, uint128_xdr denominator)
{
  uint128_t n = from_xdr(numerator);
  uint128_t d = from_xdr(denominator);
  uint128_t result = (n / d) + ((n % d) ? 1 : 0);
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
    if (hash_to_tx_.contains(tx_hash)) {
      // The transaction is already present in the runqueue.
      continue;
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

    const TxInfo info{.age = age,
                      .account = tx.tx.invocation.invoked_address,
                      .tx_hash = tx_hash,
                      .tx = tx};
    const auto &[it, inserted] = hash_to_tx_.insert({tx_hash, info});
    CHECK(inserted);
    CHECK(account_to_tx_[info.account].insert(&it->second).second);
    CHECK(age_to_tx_[info.age].insert(&it->second).second);
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

  const uint128_xdr to_schedule_hogs =
      averages.num_hogs[kHogAgeGroupToSchedule];
  const BlockHeight to_schedule_age =
      averages.average_ages[kHogAgeGroupToSchedule];

  const uint128_xdr already_scheduled_hogs =
      averages.num_hogs[kHogAgeGroupAlreadyScheduled];
  const BlockHeight already_scheduled_age =
      averages.average_ages[kHogAgeGroupAlreadyScheduled];

  const uint128_xdr received_hogs = averages.num_hogs[kHogAgeGroupReceived];
  const BlockHeight received_age = averages.average_ages[kHogAgeGroupReceived];

  // Update [kGroupToSchedule].
  new_averages.num_hogs[kHogAgeGroupToSchedule] =
      to_xdr(from_xdr(to_schedule_hogs) + from_xdr(already_scheduled_hogs));
  {
    // See comment above for [div_round_up()].
    uint128_xdr numerator =
        to_xdr((from_xdr(to_schedule_hogs) * to_schedule_age) +
               (from_xdr(already_scheduled_hogs) * already_scheduled_age));
    uint128_xdr denominator =
        to_xdr(from_xdr(to_schedule_hogs) + from_xdr(already_scheduled_hogs));
    new_averages.average_ages[kHogAgeGroupToSchedule] =
        from_xdr(div_round_up(numerator, denominator));
  }

  // Update [kGroupAlreadyScheduled].
  new_averages.num_hogs[kHogAgeGroupAlreadyScheduled] = received_hogs;
  new_averages.average_ages[kHogAgeGroupAlreadyScheduled] = received_age;

  // Update [kGroupReceived].
  new_averages.num_hogs[kHogAgeGroupReceived] =
      to_xdr(static_cast<uint128_t>(0));
  new_averages.average_ages[kHogAgeGroupReceived] = 0;

  return write_average_hog_age({new_averages});
}

bool
Scheduler::process_received_hogs(const ContractAddress &account_address,
                                 uint128_xdr num_received,
                                 BlockHeight current_block)
{
  HogAgeBatch ages;
  if (!read_average_hog_age({account_address}, ages)) {
    return false;
  }
  const HogAge &averages = ages[0];
  HogAge new_averages = averages;

  const uint128_xdr received_hogs = averages.num_hogs[kHogAgeGroupReceived];
  const BlockHeight received_age = averages.average_ages[kHogAgeGroupReceived];

  new_averages.num_hogs[kHogAgeGroupReceived] =
      to_xdr(from_xdr(received_hogs) + from_xdr(num_received));
  {
    // See comment above for [div_round_up()].
    uint128_xdr numerator = to_xdr((from_xdr(received_hogs) * received_age) +
                                   (from_xdr(num_received) * current_block));
    uint128_xdr denominator =
        to_xdr(from_xdr(received_hogs) + from_xdr(num_received));
    new_averages.average_ages[kHogAgeGroupReceived] =
        from_xdr(div_round_up(numerator, denominator));
  }

  return write_average_hog_age({new_averages});
}

void
Scheduler::update_accounts(const x23::Vec<ContractAddress> &accounts)
{
  for (const ContractAddress &account : accounts) {
    std::unordered_set<TxInfo *> &infos = account_to_tx_[account];
    for (TxInfo *i : infos) {
      // TODO(jhumphri): Need to update the age for all txs in [account] and
      // move them in the runqueue accordingly.
    }
  }
}

void
Scheduler::add_txs_from_info(std::unordered_set<TxInfo *> &info, size_t num,
                             x23::Vec<SignedTransactionHash> &hashes,
                             x23::Vec<SignedTransaction> &txs)
{
  for (auto it = info.begin(); it != info.end();) {
    TxInfo &i = **it;
    if (num-- == 0) {
      break;
    }

    hashes.push_back(i.tx_hash);
    txs.push_back(i.tx);

    CHECK_EQ(account_to_tx_[i.account].count(&i), 1UL);
    CHECK_EQ(hash_to_tx_.count(i.tx_hash), 1UL);

    CHECK_EQ(account_to_tx_[i.account].erase(&i), 1UL);
    if (account_to_tx_[i.account].empty()) {
      CHECK_EQ(account_to_tx_.erase(i.account), 1UL);
    }
    CHECK_EQ(hash_to_tx_.erase(i.tx_hash), 1UL);
    it = info.erase(it);
  }
}

std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
Scheduler::schedule(size_t block_size)
{
  x23::Vec<SignedTransactionHash> hashes;
  x23::Vec<SignedTransaction> txs;

  size_t num_scheduled = 0;
  while (num_scheduled < block_size && !age_to_tx_.empty()) {
    const size_t num_remaining = block_size - num_scheduled;

    auto &[average_age, info] = *age_to_tx_.begin();
    if (info.size() <= num_remaining) {
      num_scheduled += info.size();
      // [add_txs_from_info] removes the hashes from [hash_to_tx_],
      // [account_to_tx_], and [age_to_tx_].
      add_txs_from_info(info, info.size(), hashes, txs);
    }
    else {
      num_scheduled += num_remaining;
      // [add_txs_from_info] removes the hashes from [hash_to_tx_],
      // [account_to_tx_], and [age_to_tx_].
      add_txs_from_info(info, num_remaining, hashes, txs);
    }
    if (info.empty()) {
      CHECK_EQ(age_to_tx_.erase(average_age), 1UL);
    }
  }

  // TODO(jhumphri): At some point, we need to deduct the hogs from the account.
  // Note that we may need to reject transactions in the runqueue if the account
  // no longer has enough hogs to run them.
  return {hashes, txs};
}

} // namespace hogd
