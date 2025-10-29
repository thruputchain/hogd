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

HogAge &
NewScheduler::get_cached_hog_age(const ContractAddress &address)
{
  auto it = cached_hogs_.find(address);
  CHECK(it != cached_hogs_.end());
  return it->second;
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

  auto res = co_await db_sched_client_->dbsched_get_hog_ages(accounts_vec);
  CHECK_EQ(res.result(), DbSchedResult::DbSchedResultOK);
  for (const HogAge &age : res.hog_age_batch()) {
    cached_hogs_[age.address] = age;
  }
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
NewScheduler::process_received_hogs(const ContractAddress & /* account */,
                                    uint128_t /* num_received */,
                                    BlockHeight /* current_block */)
{
  co_return;
}

x23::Async<void>
NewScheduler::process_deducted_hogs(const ContractAddress & /* account */,
                                    uint128_t /* num_deducted */)
{
  co_return;
}

x23::Async<
    std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>>
NewScheduler::schedule(BlockHeight /* block_height */, size_t /* block_size */)
{
  std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>> ret;
  co_return ret;
}

bool
NewScheduler::find_and_remove_from_runqueue(
    const x23::Vec<SignedTransactionHash> & /* hashes */,
    x23::Vec<SignedTransaction> * /* ret */)
{
  return false;
}

} // namespace hogd
