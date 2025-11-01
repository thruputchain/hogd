#include "hogd/mempool/new_scheduler.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "sched_db.hh"
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

HogAge
NewScheduler::get_cached_hog_age(const ContractAddress &address)
{
  auto [value_ptr, lock] = accounts_.find(address);
  CHECK_PTR(value_ptr);
  return *value_ptr;
}

x23::Async<void>
NewScheduler::read_average_hog_ages(const x23::Vec<ContractAddress> &accounts)
{
  auto res = co_await db_sched_client_->dbsched_get_hog_ages(accounts);
  CHECK_EQ(res.result(), DbSchedResult::DbSchedResultOK);
  for (const HogAge &age : res.hog_age_batch()) {
    auto [value_in_map, lock] = accounts_[age.address];
    value_in_map = age;
  }
}

x23::Async<std::unordered_set<ContractAddress>>
NewScheduler::read_average_hog_ages(const x23::Vec<SignedTransaction> &txs)
{
  std::unordered_set<ContractAddress> accounts;
  for (const SignedTransaction &tx : txs) {
    accounts.insert(tx.tx.invocation.invoked_address);
  }
  x23::Vec<ContractAddress> accounts_vec;
  accounts_vec.reserve(accounts.size());
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
  co_return accounts;
}

x23::Async<std::unordered_set<ContractAddress>>
NewScheduler::read_average_hog_ages(const HogsTransferred &transfers)
{
  std::unordered_set<ContractAddress> accounts;
  for (const Delta &delta : transfers.deltas) {
    accounts.insert(delta.address);
  }
  // TODO(jhumphri): This is the same as the code above.
  x23::Vec<ContractAddress> accounts_vec;
  accounts_vec.reserve(accounts.size());
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
  co_return accounts;
}

NewScheduler::AccountKey
NewScheduler::get_account_key(const ContractAddress &account)
{
  const HogAge &hog_ages = get_cached_hog_age(account);
  BlockHeight average_hog_age = hog_ages.average_ages[kHogAgeGroupToSchedule];
  return {.average_hog_age = average_hog_age, .account = account};
}

x23::Async<void>
NewScheduler::add_txs(const x23::Vec<SignedTransaction> &txs)
{
  // Careful! [read_average_hog_ages] itself calls [co_await], so we cannot hide
  // its overhead by removing the [co_await] below, doing other work, and then
  // calling [co_await] on the handle. We instead need to execute
  // [read_average_hog_ages] on a separate thread.
  std::unordered_set<ContractAddress> accounts =
      co_await read_average_hog_ages(txs);

  x23::Vec<SignedTransactionHash> tx_hashes;
  tx_hashes.resize(txs.size());
  auto timer = std::make_unique<ElapsedTimer>();
  constexpr size_t kNumThreads = 10;
  size_t hashes_per_thread = txs.size() / kNumThreads;
  if (txs.size() % kNumThreads) {
    // Have each thread do an extra hash so that there are no hashes leftover.
    // Note that some of the threads may end up doing more work than other
    // threads.
    //
    // e.g.,
    // If there are 1,000 hashes and 128 threads:
    // 1000 / 128 = 7. 1000 % 128 != 0, so we add one to 7. Thus,
    // [hashes_per_thread] = 8.
    // Threads 0-124 will do 8 hashes.
    // Threads 125, 126, and 127 will do *no* hashes.
    hashes_per_thread++;
  }

  std::array<x23::Awaitable<void>, kNumThreads> awaitables;
  for (size_t i = 0; i < kNumThreads; i++) {
    [](size_t coro_idx, auto cb, const x23::Vec<hogd::SignedTransaction> &txs,
       x23::Vec<SignedTransactionHash> &tx_hashes,
       size_t hashes_per_thread) -> x23::Detached {
      co_await default_coropoll().pool_.bg();
      size_t start = coro_idx * hashes_per_thread;
      size_t possible_end = (coro_idx + 1) * hashes_per_thread;
      size_t end = std::min(possible_end, txs.size());
      for (size_t hash_idx = start; hash_idx < end; hash_idx++) {
        tx_hashes[hash_idx] = sha256(txs[hash_idx]);
      }
      cb();
      co_return;
    }(/*coro_idx=*/i, /*cb=*/x23::CoroCB(awaitables[i]), txs, tx_hashes,
                                 hashes_per_thread);
  }
  for (x23::Awaitable<void> &awaitable : awaitables) {
    co_await awaitable;
  }

  double tx_hashes_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  // for (const SignedTransaction &tx : txs) {
  for (size_t i = 0; i < txs.size(); i++) {
    const SignedTransaction &tx = txs[i];
    const SignedTransactionHash &tx_hash = tx_hashes[i];
    // TODO(jhumphri): This is a good parallelization opportunity! Hashes are
    // expensive to compute.
    TxValue *value = nullptr;
    {
      const auto [tx_ptr, lock] = txs_.insert(tx_hash, {.tx = tx});
      // [txs_.insert] returns a nullptr if the insertion failed.
      CHECK_PTR(tx_ptr);
      value = tx_ptr;
    }
    // We do not CHECK that [inserted.second] is true because it is okay to add
    // a tx back into the scheduler again. This can happen if the tx was
    // originally added to the scheduler, the tx was removed during validation
    // by [find_and_remove_from_runqueue()], validation failed, and the tx was
    // added back to the scheduler.

    const ContractAddress &account = tx.tx.invocation.invoked_address;
    const TxKey key{.expiry_block_height = tx.tx.uniquifier.block_expiry_height,
                    .tx_hash = tx_hash};
    auto [account_txs, lock] = account_to_tx_[account];
    // Check if the tx is already present in [account_to_tx_[account]]. If it
    // is, we are adding a duplicate tx, so we do not want to increment [size_].
    if (account_txs.insert({key, value}).second) {
      size_++;
    }
  }

  double tx_enqueue_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  for (const ContractAddress &account : accounts) {
    if (account_is_ready(account, /*remove_expired_txs=*/false)) {
      ready_accounts_.insert(get_account_key(account));
    }
  }

  double ready_accounts_duration = timer->elapsed();
  info("NewScheduler::add_txs tx_hashes_duration {}, tx_enqueue_duration {}, "
       "ready_accounts_duration {}",
       tx_hashes_duration, tx_enqueue_duration, ready_accounts_duration);
}

void
NewScheduler::cycle_if_needed(const ContractAddress &account,
                              GasAmount gas_limit)
{
  for (size_t i = 0; i < kHogAgeNumGroups - 1; i++) {
    const HogAge &hog_age = get_cached_hog_age(account);

    const uint128_t to_schedule =
        from_xdr(hog_age.num_hogs[kHogAgeGroupToSchedule]);
    const BlockHeight to_schedule_age =
        hog_age.average_ages[kHogAgeGroupToSchedule];

    const uint128_t already_scheduled =
        from_xdr(hog_age.num_hogs[kHogAgeGroupAlreadyScheduled]);
    const BlockHeight already_scheduled_age =
        hog_age.average_ages[kHogAgeGroupAlreadyScheduled];

    const uint128_t received = from_xdr(hog_age.num_hogs[kHogAgeGroupReceived]);
    const BlockHeight received_age = hog_age.average_ages[kHogAgeGroupReceived];

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
      cycle_groups(account);
    }
    else {
      break;
    }
  }
}

void
NewScheduler::cycle_groups(const ContractAddress &account)
{
  const HogAge &averages = get_cached_hog_age(account);

  HogAge new_averages{};
  new_averages.address = averages.address;

  const uint128_t to_schedule_hogs =
      from_xdr(averages.num_hogs[kHogAgeGroupToSchedule]);
  const BlockHeight to_schedule_age =
      averages.average_ages[kHogAgeGroupToSchedule];

  const uint128_t already_scheduled_hogs =
      from_xdr(averages.num_hogs[kHogAgeGroupAlreadyScheduled]);
  const BlockHeight already_scheduled_age =
      averages.average_ages[kHogAgeGroupAlreadyScheduled];

  const uint128_t received_hogs =
      from_xdr(averages.num_hogs[kHogAgeGroupReceived]);
  const BlockHeight received_age = averages.average_ages[kHogAgeGroupReceived];

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

  auto [value_ref, lock] = accounts_[account];
  value_ref = new_averages;
}

void
NewScheduler::dump_runqueue() const
{}

x23::Async<void>
NewScheduler::process_transfers(const HogsTransferred &transfers)
{
  // Careful! [read_average_hog_ages] itself calls [co_await], so we cannot hide
  // its overhead by removing the [co_await] below, doing other work, and then
  // calling [co_await] on the handle. We instead need to execute
  // [read_average_hog_ages] on a separate thread.
  co_await read_average_hog_ages(transfers);

  for (const Delta &delta : transfers.deltas) {
    int128_t num_received = from_xdr(delta.hog_delta);
    CHECK_NE(num_received, static_cast<int128_t>(0));

    const ContractAddress &account = delta.address;
    // I just wanted to make [averages] a const type fill it in with a copy from
    // of the [HogAge] struct so that I can drop the lock quickly... I could not
    // figure out a better way to do this than with a lambda.
    const HogAge averages = [&] -> HogAge {
      const auto [tmp_averages, lock] = accounts_[account];
      return tmp_averages;
    }();
    HogAge new_averages = averages;

    uint128_t received_hogs = from_xdr(averages.num_hogs[kHogAgeGroupReceived]);
    const BlockHeight received_age =
        averages.average_ages[kHogAgeGroupReceived];
    if (num_received > 0) {
      new_averages.num_hogs[kHogAgeGroupReceived] =
          to_xdr(received_hogs + num_received);
      // See comment above for [compute_average_age()].
      //
      // Note that [compute_average_age()] will never divide by 0 because we
      // check that [num_received] is not 0.
      new_averages.average_ages[kHogAgeGroupReceived] =
          compute_average_age(received_hogs, received_age, num_received,
                              transfers.block);
    }
    else {
      CHECK_LT(num_received, 0);
      auto num_received_abs = static_cast<uint128_t>(num_received * -1);

      uint128_t already_scheduled_hogs =
          from_xdr(averages.num_hogs[kHogAgeGroupAlreadyScheduled]);
      uint128_t to_schedule_hogs =
          from_xdr(averages.num_hogs[kHogAgeGroupToSchedule]);
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

    {
      auto [value_ref, lock] = accounts_[account];
      value_ref = new_averages;
    }
  }
}

bool
NewScheduler::account_is_ready(const ContractAddress &account,
                               bool remove_expired_txs)
{
  auto [txs, lock] = account_to_tx_[account];
  const TxKey lower_bound{.expiry_block_height = current_block_, .tx_hash = {}};
  auto it = txs.lower_bound(lower_bound);
  // We just check if there is a tx to run for this account. We do not check if
  // the account has enough gas units to run the tx.
  bool ret = (it != txs.end());
  if (remove_expired_txs) {
    size_t to_remove = std::distance(txs.begin(), it);
    CHECK_LE(to_remove, size_);
    size_ -= to_remove;
    txs.erase(txs.begin(), it);
  }
  return ret;
}

void
NewScheduler::move_hogs_to_already_scheduled_group(
    const ContractAddress &account, uint128_t num_hogs,
    BlockHeight block_height)
{
  HogAge age = get_cached_hog_age(account);
  const uint128_xdr already_scheduled_hogs =
      age.num_hogs[kHogAgeGroupAlreadyScheduled];
  const BlockHeight already_scheduled_age =
      age.average_ages[kHogAgeGroupAlreadyScheduled];

  // We now move the hogs to the [kHogAgeGroupAlreadyScheduled] group.
  bool empty = from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]) == 0UL;
  age.num_hogs[kHogAgeGroupAlreadyScheduled] =
      to_xdr(num_hogs + from_xdr(already_scheduled_hogs));
  if (empty) {
    age.average_ages[kHogAgeGroupAlreadyScheduled] = block_height;
  }
  else {
    // See comment above for [compute_average_age()].
    age.average_ages[kHogAgeGroupAlreadyScheduled] =
        compute_average_age(num_hogs, block_height,
                            from_xdr(already_scheduled_hogs),
                            already_scheduled_age);
  }

  {
    auto [value_ref, lock] = accounts_[account];
    value_ref = age;
  }
}

void
NewScheduler::cycle_accounts_during_schedule_helper(
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

std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
NewScheduler::schedule(size_t block_size)
{
  x23::Vec<SignedTransactionHash> tx_hashes_to_schedule;
  x23::Vec<SignedTransaction> txs_to_schedule;

  size_t num_scheduled = 0;
  std::unordered_set<ContractAddress> removed_accounts;
  // I would prefer to make [accounts_excluded_from_block] an
  // [std::unordered_set], but I have not implemented a hash function for
  // [AccountKey], so I need to use an [std::set] instead.
  std::set<AccountKey> accounts_excluded_from_block;

  const TxKey lower_bound = {.expiry_block_height = current_block_,
                             .tx_hash = {}};
  while (num_scheduled < block_size && size_) {
    if (ready_accounts_.empty()) {
      cycle_accounts_during_schedule_helper(removed_accounts, lower_bound);
      removed_accounts.clear();
      if (ready_accounts_.empty()) {
        // If there are still no ready accounts, then break.
        break;
      }
    }

    // TODO(jhumphri): This is a good place to parallelize. Each thread can
    // handle its own chunk of accounts from [ready_accounts_].
    const AccountKey account_key = *ready_accounts_.begin();
    const ContractAddress account = account_key.account;
    HogAge hog_ages = get_cached_hog_age(account);
    auto [txs, lock] = account_to_tx_[account];
    uint128_t hogs_spent_by_account = 0;
    while (num_scheduled < block_size && size_) {
      auto it = txs.lower_bound(lower_bound);
      if (it != txs.begin()) {
        // This removes expired txs.
        size_t to_remove = std::distance(txs.begin(), it);
        CHECK_LE(to_remove, size_);
        size_ -= to_remove;
        it = txs.erase(txs.begin(), it);
      }
      if (it == txs.end()) {
        CHECK_EQ(ready_accounts_.erase(account_key), 1UL);
        break;
      }
      const SignedTransactionHash &tx_hash = it->first.tx_hash;
      SignedTransaction &tx = it->second->tx;
      const uint128_t hogs_available =
          from_xdr(hog_ages.num_hogs[kHogAgeGroupToSchedule]);
      if (tx.tx.invocation.gas_limit <= hogs_available) {
        hog_ages.num_hogs[kHogAgeGroupToSchedule] =
            to_xdr(hogs_available - tx.tx.invocation.gas_limit);
        {
          auto [value_ref, lock] = accounts_[account];
          value_ref = hog_ages;
        }
        hogs_spent_by_account += tx.tx.invocation.gas_limit;

        tx_hashes_to_schedule.push_back(tx_hash);
        txs_to_schedule.push_back(tx);

        // We are deleting [it] here, so do not access [tx_hash] or [tx]
        // beyond this point!
        it = txs.erase(it);
        num_scheduled++;
        size_--;
      }
      else {
        const uint128_t all_hogs_available =
            hogs_spent_by_account +
            from_xdr(hog_ages.num_hogs[kHogAgeGroupToSchedule]) +
            from_xdr(hog_ages.num_hogs[kHogAgeGroupAlreadyScheduled]) +
            from_xdr(hog_ages.num_hogs[kHogAgeGroupReceived]);
        if (tx.tx.invocation.gas_limit > all_hogs_available) {
          // This account does not have enough hogs across all three of its
          // groups to schedule the tx, so we discard the tx.
          // We want to discourage accounts from submitting txs that cannot be
          // executed, so exclude the account from being scheduled for the
          // remainder of the block. Note that if any txs for the account are
          // already included in the block, those txs will still be executed.
          //
          // TODO(jhumphri): Do we want to penalize the account more severely?
          it = txs.erase(it);
          size_--;
          CHECK(accounts_excluded_from_block.insert(account_key).second);
        }
        else {
          // The account has enough hogs in total to run this tx, but the
          // account is going to have to be cycled once or twice. For now, we
          // just skip over this account and revisit it once it has been
          // cycled.
        }
        CHECK_EQ(ready_accounts_.erase(account_key), 1UL);
        CHECK(removed_accounts.insert(account).second);
        break;
      }
    }
    if (hogs_spent_by_account) {
      // Credit the hogs back to the [kHogAgeGroupAlreadyScheduled] group for
      // this account.
      move_hogs_to_already_scheduled_group(account, hogs_spent_by_account,
                                           current_block_);
    }
  }
  // Add [removed_accounts] back to [ready_accounts_] for each account that is
  // still ready.
  cycle_accounts_during_schedule_helper(removed_accounts, lower_bound);

  for (const AccountKey &account_key : accounts_excluded_from_block) {
    CHECK(ready_accounts_.insert(account_key).second);
  }

  increment_current_block();
  return {tx_hashes_to_schedule, txs_to_schedule};
}

bool
NewScheduler::find_and_remove_from_runqueue(
    const x23::Vec<SignedTransactionHash> &hashes,
    x23::Vec<SignedTransaction> &txs)
{
  CHECK(txs.empty());

  std::vector<TxKey> keys;
  std::vector<TxValue *> values;
  keys.reserve(hashes.size());
  values.reserve(values.size());
  txs.reserve(hashes.size());

  std::unordered_set<ContractAddress> accounts;
  for (const SignedTransactionHash &tx_hash : hashes) {
    auto [tx_value_ptr, lock] = txs_.find(tx_hash);
    if (!tx_value_ptr) {
      // We could not find all of the txs, so we put the txs we did find back
      // and return false.
      CHECK_EQ(keys.size(), values.size());
      CHECK_EQ(keys.size(), txs.size());
      CHECK_LT(keys.size(), hashes.size());
      for (size_t i = 0; i < keys.size(); i++) {
        auto [per_account_txs, lock] =
            account_to_tx_[txs[i].tx.invocation.invoked_address];
        CHECK(per_account_txs.insert({keys[i], values[i]}).second);
      }
      size_ += keys.size();
      txs.clear();
      return false;
    }

    const SignedTransaction &tx = tx_value_ptr->tx;
    const ContractAddress &account = tx.tx.invocation.invoked_address;
    const TxKey key = {.expiry_block_height =
                           tx.tx.uniquifier.block_expiry_height,
                       .tx_hash = tx_hash};
    keys.push_back(key);
    values.push_back(tx_value_ptr);
    txs.push_back(tx);
    {
      auto [txs, lock] = account_to_tx_[account];
      CHECK_EQ(txs.erase(key), 1UL);
    }
    size_--;
    accounts.insert(account);
  }
  for (const ContractAddress &account : accounts) {
    if (!account_is_ready(account, /*remove_expired_txs=*/true)) {
      CHECK_EQ(ready_accounts_.erase(get_account_key(account)), 1UL);
    }
  }
  return true;
}

} // namespace hogd
