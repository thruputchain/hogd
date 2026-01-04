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
#include "hogd/mempool/shared.h"
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
  const SignedTransactionHash &tx_hash = sha256(tx);
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

  std::array<x23::Awaitable<void>, kNumThreads> awaitables;
  for (size_t i = 0; i < kNumThreads; i++) {
    [](size_t coro_idx, auto cb,
       const x23::XdrVec<hogd::SignedTransaction> &txs, size_t txs_per_thread,
       Scheduler &scheduler) -> x23::Detached {
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
      cb();
      co_return;
    }(/*coro_idx=*/i, /*cb=*/x23::CoroCB(awaitables[i]), txs, txs_per_thread,
                             *this);
  }
  for (x23::Awaitable<void> &awaitable : awaitables) {
    co_await awaitable;
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
                                    BlockHeight block_height)
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
  std::array<x23::Awaitable<void>, kNumThreads> awaitables;
  for (size_t i = 0; i < kNumThreads; i++) {
    [](size_t coro_idx, auto cb, const HogsTransferred &transfers,
       size_t deltas_per_thread, Scheduler &scheduler) -> x23::Detached {
      co_await default_coropoll().pool_.bg();
      size_t start = coro_idx * deltas_per_thread;
      size_t possible_end = (coro_idx + 1) * deltas_per_thread;
      size_t end = std::min(possible_end, transfers.deltas.size());
      for (size_t delta_idx = start; delta_idx < end; delta_idx++) {
        scheduler.process_transfers_helper(transfers.deltas[delta_idx],
                                           transfers.block);
      }
      cb();
    }(/*coro_idx=*/i, /*cb=*/x23::CoroCB(awaitables[i]), transfers,
                                                       deltas_per_thread,
                                                       *this);
  }
  for (x23::Awaitable<void> &awaitable : awaitables) {
    co_await awaitable;
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

x23::Async<void>
Scheduler::schedule(size_t num_txs,
                    x23::XdrVec<SignedTransactionHash> &tx_hashes,
                    x23::XdrVec<SignedTransaction> &txs)
{
  // Early exit if nothing to schedule.
  if (!num_txs || ready_accounts_.empty()) {
    co_return;
  }

  // ---------------------------------------------------------------------------
  // Parallel scheduling implementation.
  //
  // This addresses the original TODO from jhumphri: "This is a good place to
  // parallelize. Each thread can handle its own chunk of accounts from
  // [ready_accounts_]."
  //
  // Algorithm:
  // 1. Outer loop: continue until we've scheduled num_txs or no more accounts
  // 2. Snapshot ready_accounts_ to a vector (preserves priority order)
  // 3. Partition accounts into chunks for parallel processing
  // 4. Each thread processes its chunk, using CAS on an atomic counter to
  //    avoid over-scheduling beyond num_txs
  // 5. Merge results in chunk order (preserves HOG age priority ordering)
  // 6. Cycle removed accounts and re-add to ready_accounts_ if they have txs
  // 7. If we haven't reached num_txs and ready_accounts_ is non-empty, repeat
  //
  // The outer loop is necessary because accounts may need cycling (moving gas
  // from AlreadyScheduled/Received groups to ToSchedule) before their txs can
  // be scheduled. Each iteration processes all ready accounts and cycles those
  // that ran out of gas.
  // ---------------------------------------------------------------------------

  // TODO(jhumphri): Consider tuning this value based on benchmarks. The
  // optimal thread count may depend on account count and system load.
  constexpr size_t kNumThreads = 128;

  // Maximum outer loop iterations to prevent unbounded spinning. This can
  // happen if concurrent add_txs() calls keep adding accounts while we're
  // scheduling. 10 iterations should be more than enough for normal cycling.
  constexpr size_t kMaxOuterIterations = 10;

  // Per-thread results. Each thread accumulates its scheduled txs and tracks
  // which accounts were removed (ran out of txs or gas) or excluded
  // (insufficient total gas).
  struct ScheduledTx {
    SignedTransactionHash hash;
    SignedTransaction tx;
  };
  struct ThreadResult {
    std::vector<ScheduledTx> scheduled;
    std::unordered_set<ContractAddress> removed_accounts;
    std::unordered_set<ContractAddress> excluded_accounts;
  };

  const TxKey lower_bound = {.expiry_block_height = current_block_,
                             .tx_hash = {}};

  // Track excluded accounts across all iterations - once excluded, stay
  // excluded for the remainder of this block.
  std::unordered_set<ContractAddress> all_excluded_accounts;

  // Atomic counter for global scheduling limit, persists across iterations.
  std::atomic<size_t> global_scheduled{0};

  // Outer loop iteration counter.
  size_t iterations = 0;

  // Outer loop: continue scheduling until we've reached num_txs or there are
  // no more accounts with txs that can be scheduled, or we hit the iteration
  // limit (to prevent unbounded spinning from concurrent add_txs()).
  while (global_scheduled.load(std::memory_order_relaxed) < num_txs &&
         !ready_accounts_.empty() &&
         size_.load(std::memory_order_relaxed) > 0 &&
         iterations < kMaxOuterIterations) {
    iterations++;

    // Step 1: Snapshot ready_accounts_ to a vector (preserves priority order).
    std::vector<AccountKey> accounts_vec(ready_accounts_.begin(),
                                         ready_accounts_.end());

    // Step 2: Partition into chunks for parallel processing.
    const auto chunks = make_chunks(accounts_vec.size(), kNumThreads);

    std::vector<ThreadResult> results(chunks.size());

    // Step 3: Parallel dispatch - each thread processes its chunk of accounts.
    std::vector<x23::Awaitable<void>> awaitables(chunks.size());
    for (size_t i = 0; i < chunks.size(); i++) {
      [](const ChunkRange &chunk, auto cb,
         const std::vector<AccountKey> &accounts_vec,
         std::atomic<size_t> &global_scheduled, size_t num_txs,
         const TxKey &lower_bound, ThreadResult &result,
         Scheduler &scheduler) -> x23::Detached {
        co_await default_coropoll().pool_.bg();

        for (size_t acct_idx = chunk.start; acct_idx < chunk.end; acct_idx++) {
          // Early exit check with relaxed ordering. This is intentionally weak
          // - we may see a stale count and process one extra account, but the
          // acquire load below (before CAS) ensures correctness when actually
          // scheduling. This pattern avoids expensive acquire loads on every
          // iteration.
          if (global_scheduled.load(std::memory_order_relaxed) >= num_txs) {
            break;
          }

          const AccountKey &account_key = accounts_vec[acct_idx];
          const ContractAddress &account = account_key.account;

          auto [hog_ages, hog_ages_lock] =
              scheduler.get_cached_hog_age(account);
          auto [txs_to_schedule, txs_lock] = scheduler.account_to_tx_[account];
          uint128_t hogs_spent_by_account = 0;
          bool account_removed = false;

          while (scheduler.size_.load(std::memory_order_relaxed) > 0) {
            // Acquire load before CAS to ensure we see all writes from threads
            // that previously incremented the counter. This synchronizes-with
            // the acq_rel CAS below, establishing a happens-before
            // relationship.
            size_t current_count =
                global_scheduled.load(std::memory_order_acquire);
            if (current_count >= num_txs) {
              break;
            }

            auto it = txs_to_schedule.lower_bound(lower_bound);

            // This removes expired txs.
            if (it != txs_to_schedule.begin()) {
              for (auto it_remove = txs_to_schedule.begin(); it_remove != it;
                   it_remove++) {
                const SignedTransactionHash &tx_hash = it_remove->first.tx_hash;
                CHECK_EQ(scheduler.txs_.remove(tx_hash), 1UL);
              }
              size_t to_remove = std::distance(txs_to_schedule.begin(), it);
              CHECK_LE(to_remove,
                       scheduler.size_.load(std::memory_order_relaxed));
              // size_ uses relaxed ordering because it's only an approximate
              // count for loop guards. The true source of truth is the
              // SynchronizedMaps (txs_, account_to_tx_), which use mutex locks.
              scheduler.size_.fetch_sub(to_remove, std::memory_order_relaxed);
              it = txs_to_schedule.erase(txs_to_schedule.begin(), it);
            }

            if (it == txs_to_schedule.end()) {
              // No more txs for this account.
              result.removed_accounts.insert(account);
              account_removed = true;
              break;
            }

            const SignedTransactionHash &tx_hash = it->first.tx_hash;
            SignedTransaction &tx = it->second->tx;
            const uint128_t hogs_available =
                from_xdr(hog_ages->num_hogs[kHogAgeGroupToSchedule]);

            if (tx.tx.invocation.gas_limit <= hogs_available) {
              // Try to claim a slot using CAS to avoid over-scheduling.
              // This ensures we never exceed num_txs even with parallel
              // threads.
              //
              // Memory ordering rationale:
              // - acq_rel on success: Ensures (1) we see all writes from
              // threads
              //   that incremented before us (acquire), and (2) our writes are
              //   visible to threads that increment after us (release).
              // - relaxed on failure: The next iteration will re-load with
              //   acquire semantics via the acquire load above. The weak CAS
              //   updates current_count automatically on failure.
              while (!global_scheduled.compare_exchange_weak(
                  current_count, current_count + 1, std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
                if (current_count >= num_txs) {
                  // Limit reached during contention, stop scheduling.
                  goto done_with_account;
                }
              }

              // Successfully claimed a slot, proceed with scheduling.
              hog_ages->num_hogs[kHogAgeGroupToSchedule] =
                  to_xdr(hogs_available - tx.tx.invocation.gas_limit);
              hogs_spent_by_account += tx.tx.invocation.gas_limit;

              result.scheduled.push_back({.hash = tx_hash, .tx = tx});

              // We are deleting [it] here, so do not access [tx_hash] or [tx]
              // beyond this point!
              it = txs_to_schedule.erase(it);
              scheduler.size_.fetch_sub(1, std::memory_order_relaxed);
            }
            else {
              // Not enough gas in ToSchedule group.
              const uint128_t all_hogs_available =
                  hogs_spent_by_account +
                  from_xdr(hog_ages->num_hogs[kHogAgeGroupToSchedule]) +
                  from_xdr(hog_ages->num_hogs[kHogAgeGroupAlreadyScheduled]) +
                  from_xdr(hog_ages->num_hogs[kHogAgeGroupReceived]);

              if (tx.tx.invocation.gas_limit > all_hogs_available) {
                // This account does not have enough hogs across all three of
                // its groups to schedule the tx, so we discard the tx.
                // We want to discourage accounts from submitting txs that
                // cannot be executed, so exclude the account from being
                // scheduled for the remainder of the block. Note that if any
                // txs for the account are already included in the block, those
                // txs will still be executed.
                //
                // TODO(jhumphri): Do we want to penalize the account more
                // severely?
                it = txs_to_schedule.erase(it);
                scheduler.size_.fetch_sub(1, std::memory_order_relaxed);
                // It is possible that the account is already in
                // [excluded_accounts], so we do not CHECK that the insert was
                // successful. This happens when [add_txs()] is called in
                // parallel with [schedule()]. [add_txs()] can enqueue another
                // tx for [account], and then if [schedule()] cycles the
                // accounts via [cycle_accounts_during_schedule_helper()], we
                // end up adding [account] back into [ready_accounts_].
                result.excluded_accounts.insert(account);
              }
              else {
                // The account has enough hogs in total to run this tx, but the
                // account is going to have to be cycled once or twice. For
                // now, we just skip over this account and revisit it once it
                // has been cycled.
              }

              result.removed_accounts.insert(account);
              account_removed = true;
              break;
            }
          }

        done_with_account:
          if (hogs_spent_by_account) {
            // Credit the hogs back to the [kHogAgeGroupAlreadyScheduled] group
            // for this account.
            move_hogs_to_already_scheduled_group(*hog_ages,
                                                 hogs_spent_by_account,
                                                 scheduler.current_block_);
          }

          // If we hit the global limit and didn't exhaust this account, mark
          // it as removed so it gets re-added to ready_accounts_ later.
          if (!account_removed &&
              global_scheduled.load(std::memory_order_relaxed) >= num_txs) {
            result.removed_accounts.insert(account);
          }
        }

        cb();
        co_return;
      }(chunks[i], x23::CoroCB(awaitables[i]), accounts_vec, global_scheduled,
                               num_txs, lower_bound, results[i], *this);
    }

    // Step 4: Await all threads.
    for (size_t i = 0; i < chunks.size(); i++) {
      co_await awaitables[i];
    }

    // Step 5: Merge results in chunk order (preserves priority ordering).
    // This ensures that accounts with older HOG ages have their txs appear
    // first in the output, maintaining the fairness invariant.
    for (const auto &result : results) {
      for (const auto &stx : result.scheduled) {
        tx_hashes.push_back(stx.hash);
        txs.push_back(stx.tx);
      }
    }

    // Step 6: Collect removed and excluded accounts from all threads.
    // Also track which accounts were actually processed (touched by a thread).
    std::unordered_set<ContractAddress> removed_accounts;
    std::unordered_set<ContractAddress> processed_accounts;
    for (const auto &result : results) {
      removed_accounts.insert(result.removed_accounts.begin(),
                              result.removed_accounts.end());
      all_excluded_accounts.insert(result.excluded_accounts.begin(),
                                   result.excluded_accounts.end());
      // An account was processed if it appears in removed_accounts or
      // excluded_accounts, or if a tx was scheduled from it.
      processed_accounts.insert(result.removed_accounts.begin(),
                                result.removed_accounts.end());
      processed_accounts.insert(result.excluded_accounts.begin(),
                                result.excluded_accounts.end());
    }
    // Also mark accounts as processed if their txs were scheduled.
    for (const auto &result : results) {
      for (const auto &stx : result.scheduled) {
        processed_accounts.insert(stx.tx.tx.invocation.invoked_address);
      }
    }

    // Step 7: Update ready_accounts_.
    // Add [removed_accounts] back to [ready_accounts_] for each account that
    // is still ready. We clear and rebuild because account keys may have
    // changed (gas deductions change the effective priority).
    //
    // IMPORTANT: We must also re-add accounts that were NOT processed at all
    // because the global limit was reached before their thread started. These
    // accounts still have txs but weren't touched.
    ready_accounts_.clear();

    // First, re-add accounts that were never processed (due to global limit).
    // These accounts need to maintain their position in ready_accounts_.
    for (const auto &account_key : accounts_vec) {
      const ContractAddress &account = account_key.account;
      if (!processed_accounts.contains(account) &&
          !all_excluded_accounts.contains(account)) {
        // This account was never touched, re-add with its original key.
        if (account_is_ready(account, /*remove_expired_txs=*/false)) {
          ready_accounts_.insert(account_key);
        }
      }
    }

    // Cycle removed accounts (those that ran out of gas or hit the limit) and
    // re-add if they still have txs. Skip excluded accounts.
    for (const ContractAddress &account : removed_accounts) {
      if (all_excluded_accounts.contains(account)) {
        continue;
      }
      cycle_accounts_during_schedule_helper({account}, lower_bound);
    }

    // If we haven't reached num_txs but ready_accounts_ is empty after
    // cycling, the loop will exit on the next iteration check.
  }

  // Log a warning if we hit the iteration limit without scheduling all
  // requested txs. This can happen if concurrent add_txs() keeps adding
  // accounts, causing repeated cycling.
  if (iterations >= kMaxOuterIterations &&
      global_scheduled.load(std::memory_order_relaxed) < num_txs) {
    warn("schedule(): hit iteration limit ({}) before scheduling all {} txs "
         "(scheduled {})",
         kMaxOuterIterations, num_txs,
         global_scheduled.load(std::memory_order_relaxed));
  }

  // Re-add excluded accounts if they still have txs (for future blocks).
  for (const ContractAddress &account : all_excluded_accounts) {
    if (account_is_ready(account, /*remove_expired_txs=*/false)) {
      ready_accounts_.insert(get_account_key(account));
    }
  }

  co_return;
}

x23::Async<void>
Scheduler::remove_from_scheduler(
    x23::span<const SignedTransactionHash> tx_hashes, bool validated_block)
{
  static SynchronizedMap<ContractAddress, char> accounts;
  accounts.clear();

  ElapsedTimer timer;

  constexpr size_t kNumThreads = 120;
  const std::vector<ChunkRange> chunks =
      make_chunks(tx_hashes.size(), kNumThreads);
  std::array<x23::Awaitable<size_t>, kNumThreads> awaitables;

  for (size_t i = 0; i < chunks.size(); i++) {
    [](const ChunkRange &chunk, auto cb,
       x23::span<const SignedTransactionHash> tx_hashes,
       SynchronizedMap<ContractAddress, char> &accounts, bool validated_block,
       Scheduler &scheduler) -> x23::Detached {
      co_await default_coropoll().pool_.bg();

      x23::span<const SignedTransactionHash> hash_chunk(
          tx_hashes.begin() + chunk.start, tx_hashes.begin() + chunk.end);
      size_t num_removed_from_runqueue = 0;
      for (const SignedTransactionHash &tx_hash : hash_chunk) {
        if (validated_block) {
          auto [tx_value_ptr, txs_lock] = scheduler.txs_.find(tx_hash);
          CHECK_PTR(tx_value_ptr);

          const SignedTransaction &tx = tx_value_ptr->tx;
          const ContractAddress &account = tx.tx.invocation.invoked_address;
          const TxKey key = {.expiry_block_height =
                                 tx.tx.uniquifier.block_expiry_height,
                             .tx_hash = tx_hash};
          {
            // We only need to remove txs from the runqueue when committing a
            // validated block. Txs in a nominated block were already removed
            // from the runqueue in [schedule()]. Thus, we can skip this for
            // nominated block to save on overhead.
            auto [txs, account_to_tx_lock] = scheduler.account_to_tx_[account];
            // The tx may or may not be on the runqueue. If the tx is included
            // in a block we are *nominating*, then it has already been taken
            // off of the runqueue. If the tx is included in a block we are
            // *validating*, then it is still on the runqueue.
            if (txs.erase(key)) {
              num_removed_from_runqueue++;
            }
          }
          accounts.insert(account, 0);

          // Careful! We remove the tx from [txs_] at the bottom here because
          // once we remove it, the references above become invalidated.
          CHECK_EQ(scheduler.txs_.remove(tx_hash, txs_lock), 1UL);
        }
        else {
          CHECK_EQ(scheduler.txs_.remove(tx_hash), 1UL);
        }
      }

      cb(num_removed_from_runqueue);
      co_return;
    }(chunks[i], /*cb=*/x23::CoroCB(awaitables[i]), tx_hashes, accounts,
                             validated_block, *this);
  }
  size_t total_removed_from_runqueue = 0;
  // There may be fewer chunks that [kNumThreads], so we iterate through the
  // number of chunks. If we were to iterate through all of the awaitables,
  // we would deadlock waiting on awaitables for coroutines that do not have a
  // chunk.
  for (size_t i = 0; i < chunks.size(); i++) {
    x23::Awaitable<size_t> &awaitable = awaitables[i];
    total_removed_from_runqueue += co_await awaitable;
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
