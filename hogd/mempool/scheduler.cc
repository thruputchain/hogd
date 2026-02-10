#include "hogd/mempool/scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "sched_db.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/corosync.h"
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

std::pair<HogAge *, std::unique_lock<std::mutex>>
Scheduler::get_cached_hog_age(const ContractAddress &address)
{
  auto [value_ptr, lock] = accounts_.find(address);
  CHECK_PTR(value_ptr);
  return {value_ptr, std::move(lock)};
}

x23::Async<void>
Scheduler::read_average_hog_ages(const x23::XdrVec<ContractAddress> &accounts)
{
  auto res = co_await db_sched_client_->dbsched_get_hog_ages(accounts);
  CHECK_EQ(res.result(), DbSchedResult::DbSchedResultOK);
  for (const HogAge &age : res.hog_age_batch()) {
    auto [value_in_map, lock] = accounts_[age.address];
    value_in_map = age;
  }
}

x23::Async<std::unordered_set<ContractAddress>>
Scheduler::read_average_hog_ages(const x23::XdrVec<SignedTransaction> &txs)
{
  std::unordered_set<ContractAddress> accounts;
  for (const SignedTransaction &tx : txs) {
    accounts.insert(tx.tx.invocation.invoked_address);
  }
  x23::XdrVec<ContractAddress> accounts_vec;
  accounts_vec.reserve(accounts.size());

  x23::CoroGuard cg;
  co_await cg(global_lock_);
  for (const ContractAddress &account : accounts) {
    bool contains = false;
    {
      auto [value_ptr, lock] = accounts_.find(account);
      contains = (value_ptr != nullptr);
    }
    if (!contains) {
      accounts_vec.push_back(account);
    }
  }
  if (!accounts_vec.empty()) {
    co_await read_average_hog_ages(accounts_vec);
  }
  cg.reset();
  co_return accounts;
}

x23::Async<std::unordered_set<ContractAddress>>
Scheduler::read_average_hog_ages(const HogsTransferred &transfers)
{
  std::unordered_set<ContractAddress> accounts;
  for (const Delta &delta : transfers.deltas) {
    accounts.insert(delta.address);
  }

  // TODO(jhumphri): This is the same as the code above.
  x23::XdrVec<ContractAddress> accounts_vec;
  accounts_vec.reserve(accounts.size());

  x23::CoroGuard cg;
  co_await cg(global_lock_);
  for (const ContractAddress &account : accounts) {
    bool contains = false;
    {
      auto [value_ptr, lock] = accounts_.find(account);
      contains = (value_ptr != nullptr);
    }
    if (!contains) {
      accounts_vec.push_back(account);
    }
  }
  if (!accounts_vec.empty()) {
    co_await read_average_hog_ages(accounts_vec);
  }
  cg.reset();
  co_return accounts;
}

Scheduler::AccountKey
Scheduler::get_account_key(const ContractAddress &account)
{
  // TODO(jhumphri): If the age is 0 (due to no hogs), set the age to INT_MAX.
  const auto [hog_age, lock] = get_cached_hog_age(account);
  BlockHeight average_hog_age = hog_age->average_ages[kHogAgeGroupToSchedule];
  return {.average_hog_age = average_hog_age, .account = account};
}

bool
Scheduler::insert_tx_into_runqueue(const SignedTransactionHash &tx_hash,
                                   TxValue &tx_value)
{
  const ContractAddress &account = tx_value.tx.tx.invocation.invoked_address;
  const TxKey key{.expiry_block_height =
                      tx_value.tx.tx.uniquifier.block_expiry_height,
                  .tx_hash = tx_hash};
  auto [account_txs, lock] = account_to_tx_[account];
  // Check if the tx is already present in [account_to_tx_[account]]. If it
  // is, we are adding a duplicate tx, so we do not want to increment [size_].
  return account_txs.insert({key, &tx_value}).second;
}

bool
Scheduler::add_txs_helper(const SignedTransaction &tx)
{
  const SignedTransactionHash &tx_hash = sha256(tx.tx);
  TxValue *value = nullptr;
  {
    // The tx may already exist in [txs_] because it is okay to add a tx back
    // into the scheduler again. This can happen if the tx was originally
    // added to the scheduler, then the tx was removed during nomination by
    // [schedule()], then a validated block was committed rather than the
    // nominated block, and then the tx was added back to the scheduler.
    const auto [tx_ptr, lock] = txs_.find_or_insert(tx_hash, {.tx = tx});
    CHECK_PTR(tx_ptr);
    value = tx_ptr;
  }

  return insert_tx_into_runqueue(tx_hash, *value);
}

// TODO(jhumphri): Use [make_chunks()] below. Remember that there may be fewer
// chunks than [kNumThreads], so only [co_await] awaitables for coroutines that
// have a chunk. If we [co_await] awaitables for coroutines that do not have a
// chunk, we will deadlock.
x23::Async<void>
Scheduler::add_txs(const x23::XdrVec<SignedTransaction> &txs)
{
  // Careful! [read_average_hog_ages] itself calls [co_await], so we cannot hide
  // its overhead by removing the [co_await] below, doing other work, and then
  // calling [co_await] on the handle. We instead need to execute
  // [read_average_hog_ages] on a separate thread.
  std::unordered_set<ContractAddress> accounts =
      co_await read_average_hog_ages(txs);

  constexpr size_t kNumThreads = 128;
  size_t txs_per_thread = txs.size() / kNumThreads;
  if (txs.size() % kNumThreads) {
    // Have each thread handle an extra tx so that there are no txs leftover.
    // Note that some of the threads may end up doing more work than other
    // threads.
    //
    // e.g.,
    // If there are 1,000 txs and 128 threads:
    // 1000 / 128 = 7. 1000 % 128 != 0, so we add one to 7. Thus,
    // [txs_per_thread] = 8.
    // Threads 0-124 will handle 8 txs.
    // Threads 125, 126, and 127 will handle *no* txs.
    txs_per_thread++;
  }

  std::vector<x23::Async<void>> handles;
  handles.reserve(kNumThreads);
  for (size_t i = 0; i < kNumThreads; i++) {
    handles.push_back([](size_t coro_idx,
                         const x23::XdrVec<hogd::SignedTransaction> &txs,
                         size_t txs_per_thread,
                         Scheduler &scheduler) noexcept -> x23::Async<void> {
      co_await default_coropoll().pool_.bg();
      size_t start = coro_idx * txs_per_thread;
      size_t possible_end = (coro_idx + 1) * txs_per_thread;
      size_t end = std::min(possible_end, txs.size());

      size_t num_added = 0;
      for (size_t tx_idx = start; tx_idx < end; tx_idx++) {
        if (scheduler.add_txs_helper(txs[tx_idx])) {
          num_added++;
        }
      }
      scheduler.size_.fetch_add(num_added, std::memory_order_relaxed);
      co_return;
    }(/*coro_idx=*/i, txs, txs_per_thread, *this));
  }
  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }

  std::vector<AccountKey> account_keys;
  account_keys.reserve(accounts.size());
  // [get_account_key()] accesses [ready_accounts_], so we need to protect it
  // with the lock.
  x23::CoroGuard cg;
  co_await cg(global_lock_);
  for (const ContractAddress &account : accounts) {
    ready_accounts_.insert(get_account_key(account));
  }
}

void
Scheduler::cycle_if_needed(const ContractAddress &account, GasAmount gas_limit)
{
  for (size_t i = 0; i < kHogAgeNumGroups - 1; i++) {
    auto [hog_age, lock] = get_cached_hog_age(account);

    const uint128_t to_schedule =
        from_xdr(hog_age->num_hogs[kHogAgeGroupToSchedule]);
    const BlockHeight to_schedule_age =
        hog_age->average_ages[kHogAgeGroupToSchedule];

    const uint128_t already_scheduled =
        from_xdr(hog_age->num_hogs[kHogAgeGroupAlreadyScheduled]);
    const BlockHeight already_scheduled_age =
        hog_age->average_ages[kHogAgeGroupAlreadyScheduled];

    const uint128_t received =
        from_xdr(hog_age->num_hogs[kHogAgeGroupReceived]);
    const BlockHeight received_age =
        hog_age->average_ages[kHogAgeGroupReceived];

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
    //    [kHogAgeGroupToSchedule] group. These are the first two operands of
    //    the boolean OR statement.
    //
    // 2. The [kHogAgeGroupToSchedule] group does not have enough hogs to
    //    schedule a tx that requires [gas_limit] hogs, but cycling will add
    //    enough hogs to the group to schedule it. This is the third operand of
    //    the boolean OR statement.
    if (already_scheduled_average_older || combined_averages_older ||
        gas_limit > to_schedule) {
      cycle_groups(*hog_age);
    }
    else {
      break;
    }
  }
}

void
Scheduler::cycle_groups(HogAge &current_averages)
{
  HogAge new_averages{};
  new_averages.address = current_averages.address;

  const uint128_t to_schedule_hogs =
      from_xdr(current_averages.num_hogs[kHogAgeGroupToSchedule]);
  const BlockHeight to_schedule_age =
      current_averages.average_ages[kHogAgeGroupToSchedule];

  const uint128_t already_scheduled_hogs =
      from_xdr(current_averages.num_hogs[kHogAgeGroupAlreadyScheduled]);
  const BlockHeight already_scheduled_age =
      current_averages.average_ages[kHogAgeGroupAlreadyScheduled];

  const uint128_t received_hogs =
      from_xdr(current_averages.num_hogs[kHogAgeGroupReceived]);
  const BlockHeight received_age =
      current_averages.average_ages[kHogAgeGroupReceived];

  // Update [kGroupToSchedule].
  new_averages.num_hogs[kHogAgeGroupToSchedule] =
      to_xdr(to_schedule_hogs + already_scheduled_hogs);
  if (to_schedule_hogs) {
    // See comment above for [compute_average_age()].
    new_averages.average_ages[kHogAgeGroupToSchedule] =
        compute_average_age(to_schedule_hogs, to_schedule_age,
                            already_scheduled_hogs, already_scheduled_age);
  }
  else {
    new_averages.average_ages[kHogAgeGroupToSchedule] = already_scheduled_age;
  }

  // Update [kGroupAlreadyScheduled].
  if (received_hogs) {
    new_averages.num_hogs[kHogAgeGroupAlreadyScheduled] = to_xdr(received_hogs);
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

  current_averages = new_averages;
}

void
Scheduler::dump_runqueue()
{
  size_t actual_size = 0;
  account_to_tx_.for_each(
      [&actual_size](
          const std::pair<ContractAddress, const std::map<TxKey, TxValue *>>
              &kv) { actual_size += kv.second.size(); });
  info("dump_runqueue(), size() = {}, actual size = {}", size(), actual_size);
}

void
Scheduler::process_transfers_helper(const Delta &delta,
                                    BlockHeight block_height) noexcept
{
  int128_t num_received = from_xdr(delta.hog_delta);
  CHECK_NE(num_received, static_cast<int128_t>(0));

  const ContractAddress &account = delta.address;
  auto [averages, lock] = get_cached_hog_age(account);
  HogAge new_averages = *averages;

  uint128_t received_hogs = from_xdr(averages->num_hogs[kHogAgeGroupReceived]);
  const BlockHeight received_age = averages->average_ages[kHogAgeGroupReceived];
  if (num_received > 0) {
    new_averages.num_hogs[kHogAgeGroupReceived] =
        to_xdr(received_hogs + num_received);
    // See comment above for [compute_average_age()].
    //
    // Note that [compute_average_age()] will never divide by 0 because we check
    // that [num_received] is not 0.
    new_averages.average_ages[kHogAgeGroupReceived] =
        compute_average_age(received_hogs, received_age, num_received,
                            block_height);
  }
  else {
    CHECK_LT(num_received, 0);
    auto num_received_abs = static_cast<uint128_t>(num_received * -1);

    uint128_t already_scheduled_hogs =
        from_xdr(averages->num_hogs[kHogAgeGroupAlreadyScheduled]);
    uint128_t to_schedule_hogs =
        from_xdr(averages->num_hogs[kHogAgeGroupToSchedule]);
    CHECK_LE(num_received_abs,
             to_schedule_hogs + already_scheduled_hogs + received_hogs);

    uint128_t to_deduct = std::min(num_received_abs, received_hogs);
    num_received_abs -= to_deduct;
    received_hogs -= to_deduct;

    if (num_received_abs > 0) {
      to_deduct = std::min(num_received_abs, already_scheduled_hogs);
      num_received_abs -= to_deduct;
      already_scheduled_hogs -= to_deduct;
    }
    if (num_received_abs > 0) {
      to_deduct = std::min(num_received_abs, to_schedule_hogs);
      num_received_abs -= to_deduct;
      to_schedule_hogs -= to_deduct;
    }
    CHECK_EQ(num_received_abs, 0UL);

    new_averages.num_hogs[kHogAgeGroupToSchedule] = to_xdr(to_schedule_hogs);
    new_averages.num_hogs[kHogAgeGroupAlreadyScheduled] =
        to_xdr(already_scheduled_hogs);
    new_averages.num_hogs[kHogAgeGroupReceived] = to_xdr(received_hogs);
  }

  // I really want [averages] to be a const so that I do not accidentally mutate
  // it above. Thus, I keep it const and I do a const cast below since I am sure
  // I want to write it here.
  *const_cast<HogAge *>(averages) = new_averages;
}

x23::Async<void>
Scheduler::process_transfers(const HogsTransferred &transfers)
{
  // Careful! [read_average_hog_ages] itself calls [co_await], so we cannot hide
  // its overhead by removing the [co_await] below, doing other work, and then
  // calling [co_await] on the handle. We instead need to execute
  // [read_average_hog_ages] on a separate thread.
  co_await read_average_hog_ages(transfers);

  constexpr size_t kNumThreads = 100;
  size_t deltas_per_thread = transfers.deltas.size() / kNumThreads;
  if (transfers.deltas.size() % kNumThreads) {
    // Have each thread handle an extra delta so that there are no deltas
    // leftover. Note that some of the threads may end up doing more work than
    // other threads.
    //
    // e.g.,
    // If there are 1,000 deltas and 128 threads:
    // 1000 / 128 = 7. 1000 % 128 != 0, so we add one to 7. Thus,
    // [deltas_per_thread] = 8.
    // Threads 0-124 will handle 8 detlas.
    // Threads 125, 126, and 127 will handle *no* deltas.
    deltas_per_thread++;
  }

  x23::CoroGuard cg;
  co_await cg(global_lock_);
  std::vector<x23::Async<void>> handles;
  handles.reserve(kNumThreads);
  for (size_t i = 0; i < kNumThreads; i++) {
    handles.push_back([](size_t coro_idx, const HogsTransferred *transfers,
                         size_t deltas_per_thread,
                         Scheduler *scheduler) noexcept -> x23::Async<void> {
      co_await default_coropoll().pool_.bg();
      size_t start = coro_idx * deltas_per_thread;
      size_t possible_end = (coro_idx + 1) * deltas_per_thread;
      size_t end = std::min(possible_end, transfers->deltas.size());
      for (size_t delta_idx = start; delta_idx < end; delta_idx++) {
        scheduler->process_transfers_helper(transfers->deltas[delta_idx],
                                            transfers->block);
      }
      co_return;
    }(/*coro_idx=*/i, &transfers, deltas_per_thread, this));
  }
  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }
}

bool
Scheduler::account_is_ready(const ContractAddress &account,
                            bool remove_expired_txs)
{
  auto [txs, lock] = account_to_tx_[account];
  const TxKey lower_bound{.expiry_block_height = current_block_, .tx_hash = {}};
  auto lower_it = txs.lower_bound(lower_bound);
  // We just check if there is a tx to run for this account. We do not check if
  // the account has enough gas units to run the tx.
  bool ret = (lower_it != txs.end());
  if (remove_expired_txs) {
    for (auto it = txs.begin(); it != lower_it; it++) {
      const SignedTransactionHash &tx_hash = it->first.tx_hash;
      CHECK_EQ(txs_.remove(tx_hash), 1UL);
    }
    size_t to_remove = std::distance(txs.begin(), lower_it);
    CHECK_LE(to_remove, size_.load(std::memory_order_relaxed));
    size_ -= to_remove;
    txs.erase(txs.begin(), lower_it);
  }
  return ret;
}

void
Scheduler::move_hogs_to_already_scheduled_group(HogAge &hog_ages,
                                                uint128_t num_hogs,
                                                BlockHeight block_height)
{
  const uint128_xdr already_scheduled_hogs =
      hog_ages.num_hogs[kHogAgeGroupAlreadyScheduled];
  const BlockHeight already_scheduled_age =
      hog_ages.average_ages[kHogAgeGroupAlreadyScheduled];

  // We now move the hogs to the [kHogAgeGroupAlreadyScheduled] group.
  bool empty = from_xdr(hog_ages.num_hogs[kHogAgeGroupAlreadyScheduled]) == 0UL;
  hog_ages.num_hogs[kHogAgeGroupAlreadyScheduled] =
      to_xdr(num_hogs + from_xdr(already_scheduled_hogs));
  if (empty) {
    hog_ages.average_ages[kHogAgeGroupAlreadyScheduled] = block_height;
  }
  else {
    // See comment above for [compute_average_age()].
    hog_ages.average_ages[kHogAgeGroupAlreadyScheduled] =
        compute_average_age(num_hogs, block_height,
                            from_xdr(already_scheduled_hogs),
                            already_scheduled_age);
  }
}

void
Scheduler::cycle_accounts_during_schedule_helper(
    const std::unordered_set<ContractAddress> &accounts,
    const TxKey &lower_bound)
{
  for (const ContractAddress &account : accounts) {
    // TODO(jhumphri): This is the same code as in [account_is_ready], but
    // we have it here because we need the iterator. Clean this up.
    GasAmount gas_limit;
    {
      const auto [txs, lock] = account_to_tx_[account];
      auto it = txs.lower_bound(lower_bound);
      if (it == txs.end()) {
        continue;
      }
      const SignedTransaction &tx = it->second->tx;
      gas_limit = tx.tx.invocation.gas_limit;
    }

    cycle_if_needed(account, gas_limit);
    if (account_is_ready(account, /*remove_expired_txs=*/false)) {
      CHECK(ready_accounts_.insert(get_account_key(account)).second);
    }
  }
}

void
Scheduler::schedule(size_t num_txs,
                    x23::XdrVec<SignedTransactionHash> &tx_hashes,
                    x23::XdrVec<SignedTransaction> &txs)
{
  size_t num_scheduled = 0;
  static std::unordered_set<ContractAddress> removed_accounts;
  static std::unordered_set<ContractAddress> accounts_excluded_from_block;
  removed_accounts.clear();
  accounts_excluded_from_block.clear();

  const TxKey lower_bound = {.expiry_block_height = current_block_,
                             .tx_hash = {}};
  auto account_it = ready_accounts_.begin();
  while (num_scheduled < num_txs && size_.load(std::memory_order_relaxed)) {
    if (account_it == ready_accounts_.end()) {
      ready_accounts_.clear();
      cycle_accounts_during_schedule_helper(removed_accounts, lower_bound);
      removed_accounts.clear();
      account_it = ready_accounts_.begin();
      if (ready_accounts_.empty()) {
        // If there are still no ready accounts, then break.
        break;
      }
    }

    // TODO(jhumphri): This is a good place to parallelize. Each thread can
    // handle its own chunk of accounts from [ready_accounts_].
    const AccountKey account_key = *account_it;
    const ContractAddress account = account_key.account;
    auto [hog_ages, hog_ages_lock] = get_cached_hog_age(account);
    auto [txs_to_schedule, txs_lock] = account_to_tx_[account];
    uint128_t hogs_spent_by_account = 0;
    while (num_scheduled < num_txs && size_.load(std::memory_order_relaxed)) {
      auto it = txs_to_schedule.lower_bound(lower_bound);
      if (it != txs_to_schedule.begin()) {
        // This removes expired txs.
        for (auto it_remove = txs_to_schedule.begin(); it_remove != it;
             it_remove++) {
          const SignedTransactionHash &tx_hash = it_remove->first.tx_hash;
          CHECK_EQ(txs_.remove(tx_hash), 1UL);
        }
        size_t to_remove = std::distance(txs_to_schedule.begin(), it);
        CHECK_LE(to_remove, size_.load(std::memory_order_relaxed));
        size_ -= to_remove;
        it = txs_to_schedule.erase(txs_to_schedule.begin(), it);
      }
      if (it == txs_to_schedule.end()) {
        account_it++;
        CHECK(removed_accounts.insert(account).second);
        break;
      }
      const SignedTransactionHash &tx_hash = it->first.tx_hash;
      SignedTransaction &tx = it->second->tx;
      const uint128_t hogs_available =
          from_xdr(hog_ages->num_hogs[kHogAgeGroupToSchedule]);
      if (tx.tx.invocation.gas_limit <= hogs_available) {
        hog_ages->num_hogs[kHogAgeGroupToSchedule] =
            to_xdr(hogs_available - tx.tx.invocation.gas_limit);
        hogs_spent_by_account += tx.tx.invocation.gas_limit;

        tx_hashes.push_back(tx_hash);
        txs.push_back(tx);

        // We are deleting [it] here, so do not access [tx_hash] or [tx]
        // beyond this point!
        it = txs_to_schedule.erase(it);
        num_scheduled++;
        size_.fetch_sub(1, std::memory_order_relaxed);
      }
      else {
        const uint128_t all_hogs_available =
            hogs_spent_by_account +
            from_xdr(hog_ages->num_hogs[kHogAgeGroupToSchedule]) +
            from_xdr(hog_ages->num_hogs[kHogAgeGroupAlreadyScheduled]) +
            from_xdr(hog_ages->num_hogs[kHogAgeGroupReceived]);
        if (tx.tx.invocation.gas_limit > all_hogs_available) {
          // This account does not have enough hogs across all three of its
          // groups to schedule the tx, so we discard the tx.
          // We want to discourage accounts from submitting txs that cannot be
          // executed, so exclude the account from being scheduled for the
          // remainder of the block. Note that if any txs for the account are
          // already included in the block, those txs will still be executed.
          //
          // TODO(jhumphri): Do we want to penalize the account more severely?
          it = txs_to_schedule.erase(it);
          size_.fetch_sub(1, std::memory_order_relaxed);
          // It is possible that the account is already in
          // [accounts_excluded_from_block], so we do not CHECK that the
          // insert was successful. This happens when [add_txs()] above is
          // called in parallel with [schedule()]. [add_txs()] can enqueue
          // another tx for [account], and then if [schedule()] cycles the
          // accounts above via [cycle_accounts_during_schedule_helper()], we
          // end up adding [account] back into [ready_accounts_].
          accounts_excluded_from_block.insert(account);
        }
        else {
          // The account has enough hogs in total to run this tx, but the
          // account is going to have to be cycled once or twice. For now, we
          // just skip over this account and revisit it once it has been
          // cycled.
        }
        account_it++;
        CHECK(removed_accounts.insert(account).second);
        break;
      }
    }
    if (hogs_spent_by_account) {
      // Credit the hogs back to the [kHogAgeGroupAlreadyScheduled] group for
      // this account.
      move_hogs_to_already_scheduled_group(*hog_ages, hogs_spent_by_account,
                                           current_block_);
    }
  }
  // Add [removed_accounts] back to [ready_accounts_] for each account that is
  // still ready.
  ready_accounts_.erase(ready_accounts_.begin(), account_it);
  cycle_accounts_during_schedule_helper(removed_accounts, lower_bound);

  for (const ContractAddress &account : accounts_excluded_from_block) {
    if (account_is_ready(account, /*remove_expired_txs=*/false)) {
      CHECK(ready_accounts_.insert(get_account_key(account)).second);
    }
  }
}

x23::Async<void>
Scheduler::remove_from_scheduler(
    x23::span<const SignedTransactionHash> tx_hashes, bool validated_block)
{
  co_await default_coropoll().pool_.bg();

  static SynchronizedMap<ContractAddress, char> accounts;
  accounts.clear();

  ElapsedTimer timer;

  constexpr size_t kNumThreads = 120;
  const std::vector<ChunkRange> chunks =
      make_chunks(tx_hashes.size(), kNumThreads);
  std::vector<x23::Async<size_t>> handles;
  handles.reserve(chunks.size());

  x23::CoroGuard cg;
  co_await cg(global_lock_);

  // NOLINTNEXTLINE(modernize-loop-convert)
  for (size_t i = 0; i < chunks.size(); i++) {
    handles.push_back([](const ChunkRange *chunk,
                         x23::span<const SignedTransactionHash> tx_hashes,
                         SynchronizedMap<ContractAddress, char> &accounts,
                         bool validated_block,
                         Scheduler &scheduler) noexcept -> x23::Async<size_t> {
      co_await default_coropoll().pool_.bg();

      x23::span<const SignedTransactionHash> hash_chunk(
          tx_hashes.begin() + chunk->start, tx_hashes.begin() + chunk->end);
      size_t num_removed_from_runqueue = 0;
      for (const SignedTransactionHash &tx_hash : hash_chunk) {
        if (validated_block) {
          // We only need to remove txs from the runqueue when committing a
          // validated block. Txs in a nominated block were already removed from
          // the runqueue in [schedule()]. Thus, we can skip this for nominated
          // blocks to save on overhead.
          auto [tx_value_ptr, txs_lock] = scheduler.txs_.find(tx_hash);
          CHECK_PTR(tx_value_ptr);

          const SignedTransaction &tx = tx_value_ptr->tx;
          const ContractAddress &account = tx.tx.invocation.invoked_address;
          const TxKey key = {.expiry_block_height =
                                 tx.tx.uniquifier.block_expiry_height,
                             .tx_hash = tx_hash};
          {
            auto [txs, account_to_tx_lock] = scheduler.account_to_tx_[account];
            // The tx may or may not be on the runqueue. If the tx is included
            // in a block we are *nominating*, then it has already been taken
            // off of the runqueue. If the tx is included in a block we are
            // *validating*, then it is still on the runqueue.
            if (txs.erase(key)) {
              num_removed_from_runqueue++;
              accounts.insert(account, 0);
            }
          }

          // Careful! We remove the tx from [txs_] at the bottom here because
          // once we remove it, the references above become invalidated.
          CHECK_EQ(scheduler.txs_.remove(tx_hash, txs_lock), 1UL);
        }
        else {
          CHECK_EQ(scheduler.txs_.remove(tx_hash), 1UL);
        }
      }

      co_return num_removed_from_runqueue;
    }(&chunks[i], tx_hashes, accounts, validated_block, *this));
  }
  size_t total_removed_from_runqueue = 0;
  // There may be fewer chunks that [kNumThreads], so we iterate through the
  // number of chunks. If we were to iterate through all of the awaitables,
  // we would deadlock waiting on awaitables for coroutines that do not have a
  // chunk.
  for (size_t i = 0; i < chunks.size(); i++) {
    x23::Async<size_t> &handle = handles[i];
    total_removed_from_runqueue += co_await handle;
  }
  double a = timer.elapsed();

  accounts.for_each([this](const std::pair<ContractAddress, char> &kv) {
    const ContractAddress &account = kv.first;
    if (!account_is_ready(account, /*remove_expired_txs=*/true)) {
      CHECK_EQ(ready_accounts_.erase(get_account_key(account)), 1UL);
    }
  });
  double b = timer.elapsed();
  size_.fetch_sub(total_removed_from_runqueue, std::memory_order_relaxed);
  double c = timer.elapsed();

  info("remove_from_scheduler a {}, b-a {}, c-b {}", a, b - a, c - b);
}

} // namespace hogd
