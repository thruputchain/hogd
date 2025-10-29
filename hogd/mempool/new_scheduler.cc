#include "hogd/mempool/new_scheduler.h"

#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "db_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
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

HogAge &
NewScheduler::get_cached_hog_age(const ContractAddress &address)
{
  auto it = cached_hogs_.find(address);
  CHECK(it != cached_hogs_.end());
  return it->second;
}

x23::Async<void>
NewScheduler::read_average_hog_ages(const x23::Vec<ContractAddress> &accounts)
{
  auto res = co_await db_sched_client_->dbsched_get_hog_ages(accounts);
  CHECK_EQ(res.result(), DbSchedResult::DbSchedResultOK);
  for (const HogAge &age : res.hog_age_batch()) {
    cached_hogs_[age.address] = age;
  }
}

x23::Async<void>
NewScheduler::read_average_hog_ages(const x23::Vec<SignedTransaction> &txs)
{
  std::unordered_set<ContractAddress> accounts;
  for (const SignedTransaction &tx : txs) {
    accounts.insert(tx.tx.invocation.invoked_address);
  }
  x23::Vec<ContractAddress> accounts_vec;
  accounts_vec.reserve(accounts.size());
  for (const ContractAddress &account : accounts) {
    if (!cached_hogs_.contains(account)) {
      accounts_vec.push_back(account);
    }
  }
  if (accounts_vec.empty()) {
    co_return;
  }
  co_await read_average_hog_ages(accounts_vec);
}

x23::Async<void>
NewScheduler::read_average_hog_ages(const HogsTransferred &transfers)
{
  std::unordered_set<ContractAddress> accounts;
  for (const Delta &delta : transfers.deltas) {
    accounts.insert(delta.address);
  }
  // TODO(jhumphri: This is the same as the code above.
  x23::Vec<ContractAddress> accounts_vec;
  accounts_vec.reserve(accounts.size());
  for (const ContractAddress &account : accounts) {
    if (!cached_hogs_.contains(account)) {
      accounts_vec.push_back(account);
    }
  }
  if (accounts_vec.empty()) {
    co_return;
  }
  co_await read_average_hog_ages(accounts_vec);
}

x23::Async<void>
NewScheduler::add_txs(const SignedTransactionBatch &txs)
{
  // Careful! [read_average_hog_ages] itself calls [co_await], so we cannot hide
  // its overhead by removing the [co_await] below, doing other work, and then
  // calling [co_await] on the handle. We instead need to execute
  // [read_average_hog_ages] on a separate thread.
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
    const HogAge &averages = cached_hogs_[account];
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
      uint128_t num_received_abs = static_cast<uint128_t>(num_received * -1);

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

    cached_hogs_[account] = new_averages;
  }
}

x23::Async<
    std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>>
NewScheduler::schedule(BlockHeight /* block_height */, size_t /* block_size */)
{
  std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>> ret;
  co_return ret;
}

x23::Vec<std::optional<SignedTransaction>>
NewScheduler::find_and_remove_from_runqueue(
    const x23::Vec<SignedTransactionHash> &hashes)
{
  x23::Vec<std::optional<SignedTransaction>> ret;
  ret.reserve(hashes.size());

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
  }
  return ret;
}

} // namespace hogd
