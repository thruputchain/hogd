#include "scheduler.h"

#include <algorithm>
#include <print>
#include <random>
#include <unordered_set>
#include <utility>

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

namespace hogd {
namespace {

BlockHeight
compute_average_age(uint128_t num_hogs_a, BlockHeight age_a,
                    uint128_t num_hogs_b, BlockHeight age_b)
{
  uint128_t numerator = (num_hogs_a * age_a) + (num_hogs_b * age_b);
  uint128_t denominator = num_hogs_a + num_hogs_b;
  CHECK_GT(denominator, 0UL);
  // When doing division, we introduce imprecision into our calculations. Thus,
  // we always round the result down (i.e., we do integer division) to avoid
  // benefitting the account. This helps to ensure that users are not
  // incentivized to waste network storage or compute, such as by spreading HOGs
  // among multiple accounts.
  return numerator / denominator;
}

} // namespace

bool
Scheduler::read_average_hog_age(const ContractAddressBatch &addresses,
                                bool force)
{
  ContractAddressBatch to_send;
  if (force) {
    to_send = addresses;
  }
  else {
    for (const ContractAddress &address : addresses) {
      if (!cached_hogs_.contains(address)) {
        to_send.push_back(address);
      }
    }
  }
  if (to_send.empty()) {
    return true;
  }

  HogAgeResult res = db_client_.dbcoord_get_hog_ages(to_send)();
  if (res.result() != DbResult::DbResultOK) {
    return false;
  }
  for (const HogAge &age : res.hog_age_batch()) {
    cached_hogs_[age.address] = age;
  }
  return true;
}

HogAge &
Scheduler::get_cached_hog_age(const ContractAddress &address)
{
  auto it = cached_hogs_.find(address);
  CHECK(it != cached_hogs_.end());
  return it->second;
}

bool
Scheduler::write_average_hog_age(const ContractAddressBatch &addresses)
{
  HogAgeBatch ages;
  ages.reserve(addresses.size());
  for (const ContractAddress &a : addresses) {
    auto it = cached_hogs_.find(a);
    if (it == cached_hogs_.end()) {
      return false;
    }
    ages.push_back(it->second);
  }
  return db_client_.dbcoord_write_hog_ages(ages)() == DbResult::DbResultOK;
}

void
Scheduler::write_cache()
{
  for (auto &[account, age] : cached_hogs_) {
    const uint128_xdr starting_balance = age.num_hogs[kHogAgeGroupToSchedule];
    size_t delta = 0;
    for (TxInfo *info : account_to_tx_[account]) {
      delta += info->tx.tx.invocation.gas_limit;
    }
    age.num_hogs[kHogAgeGroupToSchedule] =
        to_xdr(delta + from_xdr(starting_balance));
    CHECK(write_average_hog_age({account}));
    age.num_hogs[kHogAgeGroupToSchedule] = starting_balance;
  }
}

void
Scheduler::add_txs(const SignedTransactionBatch &txs)
{
  // TODO(jhumphri): Optimize this with coroutines.
  for (const SignedTransaction &tx : txs) {
    SignedTransactionHash tx_hash = sha256(tx);
    if (hash_to_tx_.contains(tx_hash)) {
      // The transaction is already present in the runqueue.
      continue;
    }

    // The mempool should not call [add_txs()] for txs that have an
    // invalid account.
    CHECK(read_average_hog_age({tx.tx.invocation.invoked_address},
                               /*force=*/false));
    const HogAge &averages = cached_hogs_[tx.tx.invocation.invoked_address];
    BlockHeight age = averages.average_ages[kHogAgeGroupToSchedule];
    const TxInfo info{.age = age,
                      .account = tx.tx.invocation.invoked_address,
                      .tx_hash = tx_hash,
                      .tx = tx};
    const auto &[it, inserted] = hash_to_tx_.insert({tx_hash, info});
    CHECK(inserted);

    cycle_if_needed(tx);
    if (tx.tx.invocation.gas_limit <=
        from_xdr(averages.num_hogs[kHogAgeGroupToSchedule])) {
      cached_hogs_[tx.tx.invocation.invoked_address]
          .num_hogs[kHogAgeGroupToSchedule] =
          to_xdr(from_xdr(cached_hogs_[tx.tx.invocation.invoked_address]
                              .num_hogs[kHogAgeGroupToSchedule]) -
                 tx.tx.invocation.gas_limit);
      CHECK(account_to_tx_[info.account].insert(&it->second).second);
      CHECK(age_to_tx_[info.age].insert(&it->second).second);
    }
    else {
      // Not enough hogs for this tx.
      CHECK(stranded_[info.account][tx.tx.invocation.gas_limit]
                .insert(&it->second)
                .second);
    }
  }
}

void
Scheduler::cycle_if_needed(const SignedTransaction &tx)
{
  const ContractAddress &account = tx.tx.invocation.invoked_address;
  const uint64_t gas_limit = tx.tx.invocation.gas_limit;

  for (size_t i = 0; i < kHogAgeNumGroups - 1; i++) {
    const uint128_t to_schedule =
        from_xdr(cached_hogs_[account].num_hogs[kHogAgeGroupToSchedule]);
    const BlockHeight to_schedule_age =
        cached_hogs_[account].average_ages[kHogAgeGroupToSchedule];

    const uint128_t already_scheduled =
        from_xdr(cached_hogs_[account].num_hogs[kHogAgeGroupAlreadyScheduled]);
    const BlockHeight already_scheduled_age =
        cached_hogs_[account].average_ages[kHogAgeGroupAlreadyScheduled];

    const uint128_t received =
        from_xdr(cached_hogs_[account].num_hogs[kHogAgeGroupReceived]);
    const BlockHeight received_age =
        cached_hogs_[account].average_ages[kHogAgeGroupReceived];

    const uint128_t total = to_schedule + already_scheduled + received;

    bool already_scheduled_average_older = false;
    bool combined_averages_older = false;

    if (already_scheduled > 0) {
      already_scheduled_average_older =
          (already_scheduled_age > to_schedule_age);
      combined_averages_older =
          (received > 0) &&
          (compute_average_age(already_scheduled, already_scheduled_age,
                               received, received_age) > to_schedule_age);
    }
    else {
      combined_averages_older =
          (received > 0) && (received_age > to_schedule_age);
    }

    // We should cycle if either of the following is true:
    // 1. Cycling will increase the average age of the [kHogAgeGroupToSchedule]
    //    group.
    // 2. The [kHogAgeGroupToSchedule] group does not have enough hogs to
    //    schedule [tx], but cycling will add enough hogs to the group to
    //    schedule [tx].
    if (already_scheduled_average_older || combined_averages_older ||
        (to_schedule < gas_limit && total >= gas_limit)) {
      cycle_groups(account);
    }
    else {
      break;
    }
  }
}

void
Scheduler::cycle_groups(const ContractAddress &account)
{
  const HogAge &averages = cached_hogs_[account];
  HogAge new_averages;
  new_averages.address = averages.address;

  // We need to add up all of the hogs reserved by txs sitting in the runqueue.
  uint128_t reserved_hogs = 0;
  for (const TxInfo *info : account_to_tx_[account]) {
    reserved_hogs += info->tx.tx.invocation.gas_limit;
  }

  const uint128_xdr to_schedule_hogs = to_xdr(
      reserved_hogs + from_xdr(averages.num_hogs[kHogAgeGroupToSchedule]));
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
  if (from_xdr(to_schedule_hogs)) {
    // See comment above for [compute_average_age()].
    new_averages.average_ages[kHogAgeGroupToSchedule] =
        compute_average_age(from_xdr(to_schedule_hogs), to_schedule_age,
                            from_xdr(already_scheduled_hogs),
                            already_scheduled_age);
  }
  else {
    new_averages.average_ages[kHogAgeGroupToSchedule] = already_scheduled_age;
  }

  // Update [kGroupAlreadyScheduled].
  new_averages.num_hogs[kHogAgeGroupAlreadyScheduled] = received_hogs;
  new_averages.average_ages[kHogAgeGroupAlreadyScheduled] = received_age;

  // Update [kGroupReceived].
  new_averages.num_hogs[kHogAgeGroupReceived] =
      to_xdr(static_cast<uint128_t>(0));
  new_averages.average_ages[kHogAgeGroupReceived] = 0;

  cached_hogs_[account] = new_averages;
  CHECK(write_average_hog_age({account}));

  update_accounts({account});
}

void
Scheduler::dump_runqueue() const
{
  // TODO(jhumphri): Improve this.
  for (const auto &[height, tx_infos] : age_to_tx_) {
    std::println("runqueue age {}:", height);
    for (const TxInfo *info : tx_infos) {
      std::println("  - account {}", info->tx.tx.invocation.invoked_address[0]);
    }
  }
}

void
Scheduler::process_received_hogs(const ContractAddress &account_address,
                                 uint128_xdr num_received,
                                 BlockHeight current_block)
{
  if (!from_xdr(num_received)) {
    return;
  }

  CHECK(read_average_hog_age({account_address}, /*force=*/false));
  const HogAge &averages = cached_hogs_[account_address];
  HogAge new_averages = averages;

  const uint128_xdr received_hogs = averages.num_hogs[kHogAgeGroupReceived];
  const BlockHeight received_age = averages.average_ages[kHogAgeGroupReceived];

  new_averages.num_hogs[kHogAgeGroupReceived] =
      to_xdr(from_xdr(received_hogs) + from_xdr(num_received));
  {
    // See comment above for [compute_average_age()].
    //
    // Note that [compute_average_age()] will never divide by 0 because we check
    // at the top of this function that [num_received] is not 0.
    new_averages.average_ages[kHogAgeGroupReceived] =
        compute_average_age(from_xdr(received_hogs), received_age,
                            from_xdr(num_received), current_block);
  }

  cached_hogs_[account_address] = new_averages;
  // No need to call [update_accounts({account_address})] because receiving hogs
  // does not affect the [kHogAgeGroupReceived] group. So in other words,
  // receiving hogs does not affect the order in which txs are scheduled.
}

void
Scheduler::update_accounts(const x23::Vec<ContractAddress> &accounts)
{
  for (const auto &account : accounts) {
    std::unordered_set<TxInfo *> &infos = account_to_tx_[account];

    // Handle existing txs in the runqueue.
    // TODO(jhumphri): Optimize this. It is currently O(N^2).
    for (auto it = infos.begin(); it != infos.end();) {
      TxInfo *info = *it;
      const HogAge &averages = cached_hogs_[account];

      CHECK_EQ(age_to_tx_[info->age].erase(info), 1UL);
      info->age = averages.average_ages[kHogAgeGroupToSchedule];
      // The account must always have enough hogs to schedule the txs that are
      // already in the runqueue. Otherwise, something has gone wrong.
      CHECK_LE(info->tx.tx.invocation.gas_limit,
               from_xdr(averages.num_hogs[kHogAgeGroupToSchedule]));
      cached_hogs_[account].num_hogs[kHogAgeGroupToSchedule] = to_xdr(
          from_xdr(cached_hogs_[account].num_hogs[kHogAgeGroupToSchedule]) -
          info->tx.tx.invocation.gas_limit);
      CHECK(age_to_tx_[info->age].insert(info).second);
      it++;
    }

    // Handle txs that are currently stranded. We may now have enough hogs to
    // add some or all of them to the runqueue.
    HogAge &averages = cached_hogs_[account];
    for (auto gas_it = stranded_[account].begin();
         gas_it != stranded_[account].end();) {
      GasAmount gas_limit = gas_it->first;
      std::unordered_set<TxInfo *> &infos = gas_it->second;

      for (auto it = infos.begin(); it != infos.end();) {
        TxInfo *info = *it;
        CHECK_EQ(gas_limit, info->tx.tx.invocation.gas_limit);

        if (gas_limit > from_xdr(averages.num_hogs[kHogAgeGroupToSchedule])) {
          break;
        }

        it = stranded_[account][gas_limit].erase(it);
        averages.num_hogs[kHogAgeGroupToSchedule] =
            to_xdr(from_xdr(averages.num_hogs[kHogAgeGroupToSchedule]) -
                   info->tx.tx.invocation.gas_limit);
        info->age = averages.average_ages[kHogAgeGroupToSchedule];
        CHECK(account_to_tx_[account].insert(info).second);
        CHECK(age_to_tx_[info->age].insert(info).second);
      }

      if (infos.empty()) {
        gas_it = stranded_[account].erase(gas_it);
      }
      else {
        gas_it++;
      }
    }
    if (stranded_[account].empty()) {
      CHECK_EQ(stranded_.erase(account), 1UL);
    }
  }
}

void
Scheduler::add_txs_from_info(std::unordered_set<TxInfo *> &info, size_t num,
                             x23::Vec<SignedTransactionHash> &hashes,
                             x23::Vec<SignedTransaction> &txs)
{
  // TODO(jhumphri): Make this more efficient by having [age_to_tx_] use vectors
  // as values rather than unordered_sets.
  std::vector<TxInfo *> v(info.begin(), info.end());

  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(v.begin(), v.end(), g);
  for (size_t idx = 0; idx < v.size(); idx++) {
    if (num-- == 0) {
      break;
    }
    TxInfo &i = *v[idx];

    hashes.push_back(i.tx_hash);
    txs.push_back(i.tx);

    CHECK_EQ(account_to_tx_[i.account].count(&i), 1UL);
    CHECK_EQ(hash_to_tx_.count(i.tx_hash), 1UL);

    CHECK_EQ(account_to_tx_[i.account].erase(&i), 1UL);
    if (account_to_tx_[i.account].empty()) {
      CHECK_EQ(account_to_tx_.erase(i.account), 1UL);
    }
    CHECK_EQ(info.erase(v[idx]), 1UL);
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
      // [add_txs_from_info] removes the hashes from [account_to_tx_] and
      // [age_to_tx_].
      add_txs_from_info(info, info.size(), hashes, txs);
    }
    else {
      num_scheduled += num_remaining;
      // [add_txs_from_info] removes the hashes from [account_to_tx_] and
      // [age_to_tx_].
      add_txs_from_info(info, num_remaining, hashes, txs);
    }
    if (info.empty()) {
      CHECK_EQ(age_to_tx_.erase(average_age), 1UL);
    }
  }

  return {hashes, txs};
}

void
Scheduler::mark_txs_executed(const x23::Vec<SignedTransactionHash> &hashes,
                             BlockHeight block)
{
  for (const SignedTransactionHash &hash : hashes) {
    const TxInfo &info = hash_to_tx_[hash];
    HogAge &age = cached_hogs_[info.tx.tx.invocation.invoked_address];

    const uint128_xdr to_schedule_hogs =
        to_xdr(static_cast<uint128_t>(info.tx.tx.invocation.gas_limit));
    const BlockHeight to_schedule_age = block;

    const uint128_xdr already_scheduled_hogs =
        age.num_hogs[kHogAgeGroupAlreadyScheduled];
    const BlockHeight already_scheduled_age =
        age.average_ages[kHogAgeGroupAlreadyScheduled];

    bool empty = from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]) == 0UL;
    age.num_hogs[kHogAgeGroupAlreadyScheduled] =
        to_xdr(from_xdr(to_schedule_hogs) + from_xdr(already_scheduled_hogs));
    if (empty) {
      age.average_ages[kHogAgeGroupAlreadyScheduled] = to_schedule_age;
    }
    else {
      // See comment above for [compute_average_age()].
      age.average_ages[kHogAgeGroupAlreadyScheduled] =
          compute_average_age(from_xdr(to_schedule_hogs), to_schedule_age,
                              from_xdr(already_scheduled_hogs),
                              already_scheduled_age);
    }
    CHECK_EQ(hash_to_tx_.erase(hash), 1UL);
  }
}

SignedTransactionBatch
Scheduler::mark_txs_failed(const x23::Vec<SignedTransactionHash> &hashes)
{
  SignedTransactionBatch txs;
  txs.reserve(hashes.size());

  for (const SignedTransactionHash &hash : hashes) {
    auto it = hash_to_tx_.find(hash);
    CHECK(it != hash_to_tx_.end());
    TxInfo &info = it->second;

    cached_hogs_[info.tx.tx.invocation.invoked_address]
        .num_hogs[kHogAgeGroupToSchedule] =
        to_xdr(from_xdr(cached_hogs_[info.tx.tx.invocation.invoked_address]
                            .num_hogs[kHogAgeGroupToSchedule]) +
               info.tx.tx.invocation.gas_limit);
    txs.push_back(info.tx);
    CHECK_EQ(hash_to_tx_.erase(hash), 1UL);
  }
  return txs;
}

bool
Scheduler::find_and_remove_from_runqueue(
    const x23::Vec<SignedTransactionHash> &hashes,
    x23::Vec<SignedTransaction> &ret)
{
  // Check that we have a transaction for each hash. In the second for loop
  // below, we remove txs from the runqueue, so we want to make sure that we
  // will not return an error code in the middle of that loop. Otherwise, we
  // would need to add the txs back to the runqueue, which is needlessly
  // complex.
  for (const SignedTransactionHash &hash : hashes) {
    if (!hash_to_tx_.contains(hash)) {
      return false;
    }
  }

  ret.clear();
  for (const SignedTransactionHash &hash : hashes) {
    auto it = hash_to_tx_.find(hash);
    CHECK(it != hash_to_tx_.end());
    ret.push_back(it->second.tx);

    // When the if statement is true, the tx is on the runqueue. Otherwise, it
    // is stranded. We need to remove it from the correct data structures
    // accordingly.
    if (account_to_tx_[it->second.account].erase(&it->second) == 1UL) {
      CHECK_EQ(age_to_tx_[it->second.age].erase(&it->second), 1UL);
    }
    else {
      CHECK_EQ(
          stranded_[it->second.account][it->second.tx.tx.invocation.gas_limit]
              .erase(&it->second),
          1UL);
    }
  }
  return true;
}

} // namespace hogd
