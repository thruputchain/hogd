#include "scheduler.h"

#include <algorithm>
#include <functional>
#include <map>
#include <print>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23/async.h"

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
  // we always round the result up to avoid benefitting the account. This helps
  // to ensure that users are not incentivized to waste network storage or
  // compute, such as by spreading HOGs among multiple accounts.
  return (numerator / denominator) + ((numerator % denominator) ? 1 : 0);
}

} // namespace

x23::Async<bool>
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
    co_return true;
  }

  HogAgeResult res = co_await db_sched_client_.dbsched_get_hog_ages(to_send);
  if (res.result() != DbSchedResult::DbSchedResultOK) {
    co_return false;
  }
  for (const HogAge &age : res.hog_age_batch()) {
    cached_hogs_[age.address] = age;
  }
  co_return true;
}

HogAge &
Scheduler::get_cached_hog_age(const ContractAddress &address)
{
  auto it = cached_hogs_.find(address);
  CHECK(it != cached_hogs_.end());
  return it->second;
}

x23::Async<bool>
Scheduler::write_average_hog_age(const ContractAddressBatch &addresses)
{
  HogAgeBatch ages;
  ages.reserve(addresses.size());
  for (const ContractAddress &a : addresses) {
    auto it = cached_hogs_.find(a);
    if (it == cached_hogs_.end()) {
      co_return false;
    }
    ages.push_back(it->second);
  }
  co_return (co_await db_sched_client_.dbsched_write_hog_ages(ages)) ==
      DbSchedResult::DbSchedResultOK;
}

x23::Async<void>
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
    CHECK(co_await write_average_hog_age({account}));
    age.num_hogs[kHogAgeGroupToSchedule] = starting_balance;
  }
}

x23::Async<void>
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
    CHECK(co_await read_average_hog_age({tx.tx.invocation.invoked_address},
                                        /*force=*/false));
    const HogAge &averages = cached_hogs_[tx.tx.invocation.invoked_address];
    BlockHeight age = averages.average_ages[kHogAgeGroupToSchedule];
    const TxInfo info{.age = age,
                      .account = tx.tx.invocation.invoked_address,
                      .tx_hash = tx_hash,
                      .tx = tx};
    const auto &[it, inserted] = hash_to_tx_.insert({tx_hash, info});
    CHECK(inserted);

    cycle_if_needed(tx.tx.invocation.invoked_address,
                    tx.tx.invocation.gas_limit);
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
    {
      const BlockHeight expiry_height =
          info.tx.tx.uniquifier.block_expiry_height;
      CHECK(expiry_to_tx_[expiry_height].insert(&it->second).second);
    }
  }
}

x23::Async<void>
Scheduler::cycle_if_needed(const ContractAddress &account, GasAmount gas_limit)
{
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

    bool already_scheduled_average_older = false;
    bool combined_averages_older = false;

    if (already_scheduled > 0) {
      already_scheduled_average_older =
          (already_scheduled_age < to_schedule_age);
      combined_averages_older =
          (received > 0) &&
          (compute_average_age(already_scheduled, already_scheduled_age,
                               received, received_age) < to_schedule_age);
    }
    else {
      combined_averages_older =
          (received > 0) && (received_age < to_schedule_age);
    }

    // We should cycle if either of the following is true:
    // 1. Cycling will increase the average age of the
    // [kHogAgeGroupToSchedule]
    //    group.
    // 2. The [kHogAgeGroupToSchedule] group does not have enough hogs to
    //    schedule tx that requires [gas_limit] hogs, but cycling will add
    //    enough hogs to the group to schedule it.
    if (already_scheduled_average_older || combined_averages_older ||
        (account_to_tx_[account].empty() && gas_limit > to_schedule)) {
      co_await cycle_groups(account);
    }
    else {
      break;
    }
  }
}

x23::Async<void>
Scheduler::cycle_groups(const ContractAddress &account)
{
  const HogAge &averages = cached_hogs_[account];
  HogAge new_averages;
  new_averages.address = averages.address;

  // We need to add up all of the hogs reserved by txs sitting in the
  // runqueue.
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
  if (from_xdr(received_hogs)) {
    new_averages.num_hogs[kHogAgeGroupAlreadyScheduled] = received_hogs;
    new_averages.average_ages[kHogAgeGroupAlreadyScheduled] = received_age;
  }
  else {
    new_averages.num_hogs[kHogAgeGroupAlreadyScheduled] =
        to_xdr(static_cast<uint128_t>(0));
    new_averages.average_ages[kHogAgeGroupAlreadyScheduled] = 0;
  }

  // Update [kGroupReceived].
  new_averages.num_hogs[kHogAgeGroupReceived] =
      to_xdr(static_cast<uint128_t>(0));
  new_averages.average_ages[kHogAgeGroupReceived] = 0;

  cached_hogs_[account] = new_averages;
  CHECK(co_await write_average_hog_age({account}));

  update_accounts({account});
}

void
Scheduler::dump_runqueue() const
{
  for (const auto &[height, tx_infos] : age_to_tx_) {
    std::println("runqueue age {}:", height);
    for (const TxInfo *info : tx_infos) {
      std::println("  - account {}", info->tx.tx.invocation.invoked_address[0]);
    }
  }
}

x23::Async<void>
Scheduler::process_received_hogs(const ContractAddress &account,
                                 uint128_xdr num_received,
                                 BlockHeight current_block)
{
  if (!from_xdr(num_received)) {
    co_return;
  }

  CHECK(co_await read_average_hog_age({account}, /*force=*/false));
  const HogAge &averages = cached_hogs_[account];
  HogAge new_averages = averages;

  const uint128_xdr received_hogs = averages.num_hogs[kHogAgeGroupReceived];
  const BlockHeight received_age = averages.average_ages[kHogAgeGroupReceived];

  new_averages.num_hogs[kHogAgeGroupReceived] =
      to_xdr(from_xdr(received_hogs) + from_xdr(num_received));
  {
    // See comment above for [compute_average_age()].
    //
    // Note that [compute_average_age()] will never divide by 0 because we
    // check at the top of this function that [num_received] is not 0.
    new_averages.average_ages[kHogAgeGroupReceived] =
        compute_average_age(from_xdr(received_hogs), received_age,
                            from_xdr(num_received), current_block);
  }
  cached_hogs_[account] = new_averages;

  // TODO(jhumphri): Add a test for this.
  // No need to call [update_accounts({account})] because receiving hogs does
  // not affect the [kHogAgeGroupToSchedule] group. So in other words,
  // receiving hogs does not affect the order in which txs are scheduled.
  //
  // *However*, if there are no txs on the runqueue _and_ there are stranded
  // txs, then we may be able to add those txs to the runqueue with the newly
  // received hogs. We call [cycle_groups()] in this case to move the received
  // hogs into the other groups and then potentially add stranded txs to the
  // runqueue. We need to call [cycle_groups()] twice to get the
  // [kHogAgeGroupReceived] hogs into the [kHogAgeGroupToSchedule] group.
  if (account_to_tx_[account].empty() && !stranded_[account].empty()) {
    co_await cycle_groups(account);
    co_await cycle_groups(account);
  }
}

void
Scheduler::update_accounts(const x23::Vec<ContractAddress> &accounts)
{
  for (const auto &account : accounts) {
    std::unordered_set<TxInfo *> &infos = account_to_tx_[account];

    // Handle existing txs in the runqueue.
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
Scheduler::remove_tx_from_runqueue(TxInfo *info,
                                   x23::Vec<SignedTransactionHash> &hashes,
                                   x23::Vec<SignedTransaction> &txs)
{
  hashes.push_back(info->tx_hash);
  txs.push_back(info->tx);

  CHECK_EQ(account_to_tx_[info->account].erase(info), 1UL);
  if (account_to_tx_[info->account].empty()) {
    CHECK_EQ(account_to_tx_.erase(info->account), 1UL);
  }
}

void
Scheduler::get_txs_all(std::unordered_set<TxInfo *> &info,
                       x23::Vec<SignedTransactionHash> &hashes,
                       x23::Vec<SignedTransaction> &txs)
{
  for (auto it = info.begin(); it != info.end();) {
    remove_tx_from_runqueue(*it, hashes, txs);
    it = info.erase(it);
  }
}

void
Scheduler::get_txs_subset(std::unordered_set<TxInfo *> &info, size_t num,
                          x23::Vec<SignedTransactionHash> &hashes,
                          x23::Vec<SignedTransaction> &txs)
{
  while (num) {
    std::unordered_map<ContractAddress, TxInfo *> account_set;
    for (TxInfo *i : info) {
      account_set.insert({i->account, i});
    }

    std::vector<TxInfo *> to_schedule;
    to_schedule.reserve(account_set.size());
    for (const auto &[_, tx] : account_set) {
      to_schedule.push_back(tx);
    }

    {
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(to_schedule.begin(), to_schedule.end(), g);
    }

    size_t num_iterations = std::min(num, to_schedule.size());
    for (size_t i = 0; i < num_iterations; i++) {
      TxInfo *this_info = to_schedule[i];
      remove_tx_from_runqueue(this_info, hashes, txs);
      CHECK_EQ(info.erase(this_info), 1UL);
      num--;
    }
  }
}

x23::Async<void>
Scheduler::discard_expired_txs(BlockHeight current_height)
{
  x23::Vec<SignedTransactionHash> to_discard;
  for (const auto &[height, tx_infos] : expiry_to_tx_) {
    if (height >= current_height) {
      break;
    }

    for (TxInfo *info : tx_infos) {
      to_discard.push_back(info->tx_hash);
    }
  }
  co_await discard_txs(to_discard);
}

x23::Async<void>
Scheduler::discard_txs(const x23::Vec<SignedTransactionHash> &tx_hashes)
{
  std::unordered_set<ContractAddress> accounts;
  for (const SignedTransactionHash &tx_hash : tx_hashes) {
    CHECK(hash_to_tx_.contains(tx_hash));
    TxInfo &info = hash_to_tx_[tx_hash];

    if (on_runqueue(info)) {
      accounts.insert(info.tx.tx.invocation.invoked_address);
      cached_hogs_[info.tx.tx.invocation.invoked_address]
          .num_hogs[kHogAgeGroupToSchedule] =
          to_xdr(from_xdr(cached_hogs_[info.tx.tx.invocation.invoked_address]
                              .num_hogs[kHogAgeGroupToSchedule]) +
                 info.tx.tx.invocation.gas_limit);
    }

    CHECK(find_and_remove_from_runqueue(tx_hashes, /*ret=*/nullptr));
    {
      const BlockHeight expiry_height =
          info.tx.tx.uniquifier.block_expiry_height;
      CHECK_EQ(expiry_to_tx_[expiry_height].erase(&info), 1UL);
      if (expiry_to_tx_[expiry_height].empty()) {
        CHECK_EQ(expiry_to_tx_.erase(expiry_height), 1UL);
      }
    }
    CHECK_EQ(hash_to_tx_.erase(tx_hash), 1UL);
  }

  // Now that we have refunded the hogs to the [kHogAgeGroupToSchedule] group
  // for all accounts in [accounts], add any stranded txs to the runqueue if
  // possible.
  for (const ContractAddress &account : accounts) {
    update_accounts({account});
    if (account_to_tx_[account].empty() && !stranded_[account].empty()) {
      co_await cycle_groups(account);
    }
  }
}

x23::Async<
    std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>>
Scheduler::schedule(BlockHeight block_height, size_t block_size)
{
  x23::Vec<SignedTransactionHash> hashes;
  x23::Vec<SignedTransaction> txs;
  hashes.reserve(block_size);
  txs.reserve(block_size);

  // Discard any expired txs before scheduling the current block.
  co_await discard_expired_txs(block_height);

  size_t num_scheduled = 0;
  while (num_scheduled < block_size && !age_to_tx_.empty()) {
    const size_t num_remaining = block_size - num_scheduled;
    auto &[average_age, info] = *age_to_tx_.begin();

    if (info.size() <= num_remaining) {
      // All of the txs in [info] in the block, so we add all of them. Note
      // that [get_txs_all()] removes the txs from [info], so it is important
      // that we add [info.size()] to [num_scheduled] first.
      num_scheduled += info.size();
      get_txs_all(info, hashes, txs);
    }
    else {
      // We can only fit some of the txs in [info] in the block, so we need to
      // choose a subset of [info] to schedule.
      num_scheduled += num_remaining;
      get_txs_subset(info, num_remaining, hashes, txs);
    }
    if (info.empty()) {
      CHECK_EQ(age_to_tx_.erase(average_age), 1UL);
    }
  }
  co_return {hashes, txs};
}

// TODO(jhumphri): We should remove the [block] parameter. We should know the
// current block number based on the block number passed to [schedule()].
x23::Async<void>
Scheduler::mark_txs_executed(const x23::Vec<SignedTransactionHash> &hashes,
                             BlockHeight block)
{
  std::unordered_set<ContractAddress> accounts;

  for (const SignedTransactionHash &hash : hashes) {
    auto it = hash_to_tx_.find(hash);
    CHECK(it != hash_to_tx_.end());
    TxInfo &info = it->second;
    HogAge &age = cached_hogs_[info.tx.tx.invocation.invoked_address];
    accounts.insert(info.account);

    const uint128_xdr to_schedule_hogs =
        to_xdr(static_cast<uint128_t>(info.tx.tx.invocation.gas_limit));
    const BlockHeight to_schedule_age = block;

    const uint128_xdr already_scheduled_hogs =
        age.num_hogs[kHogAgeGroupAlreadyScheduled];
    const BlockHeight already_scheduled_age =
        age.average_ages[kHogAgeGroupAlreadyScheduled];

    // We now move the hogs to the [kHogAgeGroupAlreadyScheduled] group.
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
    {
      const BlockHeight expiry_height =
          info.tx.tx.uniquifier.block_expiry_height;
      CHECK_EQ(expiry_to_tx_[expiry_height].erase(&info), 1UL);
      if (expiry_to_tx_[expiry_height].empty()) {
        CHECK_EQ(expiry_to_tx_.erase(expiry_height), 1UL);
      }
    }
    CHECK_EQ(hash_to_tx_.erase(hash), 1UL);
  }
  for (const ContractAddress &account : accounts) {
    if (account_to_tx_[account].empty() && !stranded_[account].empty()) {
      co_await cycle_groups(account);
    }
  }
}

x23::Async<SignedTransactionBatch>
Scheduler::mark_txs_failed(const x23::Vec<SignedTransactionHash> &hashes)
{
  std::unordered_set<ContractAddress> accounts;
  SignedTransactionBatch txs;
  txs.reserve(hashes.size());

  for (const SignedTransactionHash &hash : hashes) {
    auto it = hash_to_tx_.find(hash);
    CHECK(it != hash_to_tx_.end());
    TxInfo &info = it->second;
    accounts.insert(info.account);

    cached_hogs_[info.tx.tx.invocation.invoked_address]
        .num_hogs[kHogAgeGroupToSchedule] =
        to_xdr(from_xdr(cached_hogs_[info.tx.tx.invocation.invoked_address]
                            .num_hogs[kHogAgeGroupToSchedule]) +
               info.tx.tx.invocation.gas_limit);
    txs.push_back(info.tx);
    {
      const BlockHeight expiry_height =
          info.tx.tx.uniquifier.block_expiry_height;
      CHECK_EQ(expiry_to_tx_[expiry_height].erase(&info), 1UL);
      if (expiry_to_tx_[expiry_height].empty()) {
        CHECK_EQ(expiry_to_tx_.erase(expiry_height), 1UL);
      }
    }
    CHECK_EQ(hash_to_tx_.erase(hash), 1UL);
  }
  // Now that we have refunded the hogs to the [kHogAgeGroupToSchedule] group
  // for all accounts in [accounts], add any stranded txs to the runqueue if
  // possible.
  for (const ContractAddress &account : accounts) {
    update_accounts({account});
    if (account_to_tx_[account].empty() && !stranded_[account].empty()) {
      co_await cycle_groups(account);
    }
  }
  co_return txs;
}

bool
Scheduler::find_and_remove_from_runqueue(
    const x23::Vec<SignedTransactionHash> &hashes,
    x23::Vec<SignedTransaction> *ret)
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

  if (ret) {
    ret->clear();
  }
  for (const SignedTransactionHash &hash : hashes) {
    auto it = hash_to_tx_.find(hash);
    CHECK(it != hash_to_tx_.end());
    if (ret) {
      ret->push_back(it->second.tx);
    }

    // When the if statement is true, the tx is on the runqueue. Otherwise, it
    // is stranded. We need to remove it from the correct data structures
    // accordingly.
    if (account_to_tx_[it->second.account].erase(&it->second) == 1UL) {
      if (account_to_tx_[it->second.account].empty()) {
        CHECK_EQ(account_to_tx_.erase(it->second.account), 1UL);
      }
      CHECK_EQ(age_to_tx_[it->second.age].erase(&it->second), 1UL);
      if (age_to_tx_[it->second.age].empty()) {
        CHECK_EQ(age_to_tx_.erase(it->second.age), 1UL);
      }
    }
    else {
      CHECK_EQ(
          stranded_[it->second.account][it->second.tx.tx.invocation.gas_limit]
              .erase(&it->second),
          1UL);
      if (stranded_[it->second.account][it->second.tx.tx.invocation.gas_limit]
              .empty()) {
        CHECK_EQ(stranded_[it->second.account].erase(
                     it->second.tx.tx.invocation.gas_limit),
                 1UL);
      }
      if (stranded_[it->second.account].empty()) {
        CHECK_EQ(stranded_.erase(it->second.account), 1UL);
      }
    }
  }
  return true;
}

x23::Async<void>
Scheduler::filter_txs_for_account(
    const ContractAddress &account,
    const std::function<bool(const SignedTransaction &tx)> &filter)
{
  x23::Vec<SignedTransactionHash> to_discard;
  {
    // Iterate through [account]'s txs that are on the runqueue.
    auto it = account_to_tx_.find(account);
    if (it != account_to_tx_.end()) {
      const std::unordered_set<TxInfo *> &txs = it->second;
      for (const TxInfo *info : txs) {
        const SignedTransaction &tx = info->tx;
        if (!filter(tx)) {
          to_discard.push_back(info->tx_hash);
        }
      }
    }
  }
  {
    // Iterate through [account]'s txs that are stranded.
    auto it = stranded_.find(account);
    if (it != stranded_.end()) {
      const std::map<GasAmount, std::unordered_set<TxInfo *>> &gas_txs =
          it->second;
      for (const auto &[_, txs] : gas_txs) {
        for (const TxInfo *info : txs) {
          const SignedTransaction &tx = info->tx;
          if (!filter(tx)) {
            to_discard.push_back(info->tx_hash);
          }
        }
      }
    }
  }
  co_await discard_txs(to_discard);
}

} // namespace hogd
