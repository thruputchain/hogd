#include "hogd/mempool/new_scheduler.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "db_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
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

HogAge &
NewScheduler::get_cached_hog_age(const ContractAddress &address)
{
  auto it = accounts_.find(address);
  CHECK(it != accounts_.end());
  return it->second;
}

x23::Async<void>
NewScheduler::read_average_hog_ages(const x23::Vec<ContractAddress> &accounts)
{
  auto res = co_await db_sched_client_->dbsched_get_hog_ages(accounts);
  CHECK_EQ(res.result(), DbSchedResult::DbSchedResultOK);
  for (const HogAge &age : res.hog_age_batch()) {
    accounts_[age.address] = age;
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
    if (!accounts_.contains(account)) {
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
    if (!accounts_.contains(account)) {
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
NewScheduler::add_txs(const SignedTransactionBatch &txs)
{
  // Careful! [read_average_hog_ages] itself calls [co_await], so we cannot hide
  // its overhead by removing the [co_await] below, doing other work, and then
  // calling [co_await] on the handle. We instead need to execute
  // [read_average_hog_ages] on a separate thread.
  std::unordered_set<ContractAddress> accounts =
      co_await read_average_hog_ages(txs);

  for (const SignedTransaction &tx : txs) {
    // TODO(jhumphri): This is a good parallelization opportunity! Hashes are
    // expensive to compute.
    const SignedTransactionHash tx_hash = sha256(tx);
    auto inserted = txs_.insert({tx_hash, {.tx = tx}});
    CHECK(inserted.second);

    const ContractAddress &account = tx.tx.invocation.invoked_address;
    const TxKey key{.expiry_block_height = tx.tx.uniquifier.block_expiry_height,
                    .tx_hash = tx_hash};
    // This is a pretty dense line of code. Basically, I just want to get a
    // pointer to the [TxValue] struct in the [txs_] map, because the [txs_] map
    // is the source of truth for txs. Let me break down
    // [&inserted.first->second].
    //
    // [inserted] is returned by [txs_.insert()] above, so it is a pair
    // containing an iterator to the KV pair in the the [txs_] map along with a
    // boolean indicating whether the insertion was successful.
    //
    // [inserted.first] is the iterator itself. The iterator points to the KV
    // pair, which is an std::pair<SignedTransactionHash, TxValue>. We
    // ultimately want to get a pointer to the [TxValue].
    //
    // To get the [TxValue], we do [inserted.first->second]. We use the
    // dereference operator because [inserted.first] is an iterator and thus
    // requires it, and we access [second] because the [TxValue] is second in
    // the pair.
    //
    // We then get the address of the [TxValue]: [&inserted.first->second].
    TxValue *value = &inserted.first->second;
    CHECK(account_to_tx_[account].insert({key, value}).second);
  }
  for (const ContractAddress &account : accounts) {
    // TODO(jhumphri): Set [current_block] correctly.
    if (account_is_ready(account)) {
      ready_accounts_.insert(get_account_key(account));
    }
  }
  size_ += txs.size();
}

void
NewScheduler::cycle_if_needed(const ContractAddress & /* account */,
                              GasAmount /* gas_limit */)
{}

void
NewScheduler::cycle_groups(const ContractAddress & /* account */)
{}

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
    if (!num_received) {
      continue;
    }

    const ContractAddress &account = delta.address;
    const HogAge &averages = accounts_[account];
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

    accounts_[account] = new_averages;
  }
}

// TODO(jhumphri): Call this function from [add_txs],
// [find_and_remove_from_runqueue], and [schedule].
bool
NewScheduler::account_is_ready(const ContractAddress &account)
{
  const std::map<TxKey, TxValue *> &txs = account_to_tx_[account];
  const TxKey lower_bound{.expiry_block_height = current_block_ + 1,
                          .tx_hash = {}};
  // We just check if there is a tx to run for this account. We do not check if
  // the account has enough gas units to run the tx.
  return txs.lower_bound(lower_bound) != txs.end();
}

void
NewScheduler::move_hogs_to_already_scheduled_group(
    const ContractAddress &account, uint128_t num_hogs,
    BlockHeight block_height)
{
  HogAge &age = get_cached_hog_age(account);
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
}

std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>
NewScheduler::schedule(size_t block_size)
{
  x23::Vec<SignedTransactionHash> tx_hashes_to_schedule;
  x23::Vec<SignedTransaction> txs_to_schedule;

  size_t num_scheduled = 0;
  size_t num_cycles = 0;
  std::unordered_map<ContractAddress, uint128_t> scheduled_accounts;
  // I would prefer to make [accounts_excluded_from_block] an
  // [std::unordered_set], but I have not implemented a hash function for
  // [AccountKey], so I need to use an [std::set] instead.
  std::set<AccountKey> accounts_excluded_from_block;

  current_block_++;
  const TxKey lower_bound = {.expiry_block_height = current_block_,
                             .tx_hash = {}};
  while (num_scheduled < block_size && size_) {
    if (ready_accounts_.empty()) {
      if (num_cycles++ >= 2) {
        break;
      }

      for (const auto &[account, _] : scheduled_accounts) {
        const std::map<TxKey, TxValue *> &txs = account_to_tx_[account];
        auto it = txs.lower_bound(lower_bound);
        if (it == txs.end()) {
          continue;
        }
        const SignedTransaction &tx = it->second->tx;
        cycle_if_needed(account, tx.tx.invocation.gas_limit);
      }
    }

    // TODO(jhumphri): This is a good place to parallelize. Each thread can
    // handle its own chunk of accounts from [ready_accounts_].
    const AccountKey account_key = *ready_accounts_.begin();
    const ContractAddress account = account_key.account;
    HogAge &hog_ages = get_cached_hog_age(account);
    std::map<TxKey, TxValue *> &txs = account_to_tx_[account];
    uint128_t hogs_spent_by_account = 0;
    while (true) {
      auto it = txs.lower_bound(lower_bound);
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
        hogs_spent_by_account += tx.tx.invocation.gas_limit;

        tx_hashes_to_schedule.push_back(tx_hash);
        txs_to_schedule.push_back(tx);
        num_scheduled++;

        // We are deleting [it] here, so do not access [tx_hash] or [tx] beyond
        // this point!
        it = txs.erase(it);
      }
      else {
        const uint128_t all_hogs_available =
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
          CHECK_EQ(ready_accounts_.erase(account_key), 1UL);
          if (it != txs.end()) {
            CHECK(accounts_excluded_from_block.insert(account_key).second);
          }
        }
        else {
          // The account has enough hogs in total to run this tx, but the
          // account is going to have to be cycled once or twice. For now, we
          // just skip over this account and revisit it once it has been cycled.
        }
        break;
      }
      if (hogs_spent_by_account) {
        scheduled_accounts[account] += hogs_spent_by_account;
      }
    }
  }
  for (const AccountKey &account_key : accounts_excluded_from_block) {
    CHECK(ready_accounts_.insert(account_key).second);
  }
  // Credit the hogs back to the [kHogAgeGroupAlreadyScheduled] group for each
  // scheduled account.
  //
  // TODO(jhumphri): This is a good place to parallelize.
  for (const auto &[account, spent_hogs] : scheduled_accounts) {
    move_hogs_to_already_scheduled_group(account, spent_hogs, current_block_);
  }

  return {tx_hashes_to_schedule, txs_to_schedule};
}

x23::Vec<std::optional<SignedTransaction>>
NewScheduler::find_and_remove_from_runqueue(
    const x23::Vec<SignedTransactionHash> &hashes)
{
  x23::Vec<std::optional<SignedTransaction>> ret;
  ret.reserve(hashes.size());

  std::unordered_set<ContractAddress> accounts;
  for (const SignedTransactionHash &tx_hash : hashes) {
    auto it = txs_.find(tx_hash);
    if (it == txs_.end()) {
      ret.push_back(std::nullopt);
      continue;
    }

    const SignedTransaction &tx = it->second.tx;
    ret.push_back(tx);

    const ContractAddress &account = tx.tx.invocation.invoked_address;
    const TxKey key = {.expiry_block_height =
                           tx.tx.uniquifier.block_expiry_height,
                       .tx_hash = tx_hash};
    // TODO(jhumphri): [find_and_remove_from_runqueue] is only called during
    // validation, so I think it is fine if the tx is not on the runqueue? Thus,
    // it is ok if [erase()] returns 0 below.
    if (account_to_tx_[account].erase(key)) {
      size_--;
    }
    accounts.insert(account);
  }
  for (const ContractAddress &account : accounts) {
    if (!account_is_ready(account)) {
      CHECK_EQ(ready_accounts_.erase(get_account_key(account)), 1UL);
    }
  }
  return ret;
}

} // namespace hogd
