#include "hogd/mempool/new_scheduler.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <random>
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
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23/async.h"

namespace hogd {

x23::Async<bool>
NewScheduler::read_average_hog_age(const ContractAddressBatch &addresses,
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

  HogAgeResult res = co_await db_sched_client_->dbsched_get_hog_ages(to_send);
  if (res.result() != DbSchedResult::DbSchedResultOK) {
    co_return false;
  }
  for (const HogAge &age : res.hog_age_batch()) {
    cached_hogs_[age.address] = age;
  }
  co_return true;
}

HogAge &
NewScheduler::get_cached_hog_age(const ContractAddress &address)
{
  auto it = cached_hogs_.find(address);
  CHECK(it != cached_hogs_.end());
  return it->second;
}

x23::Async<bool>
NewScheduler::write_average_hog_age(const ContractAddressBatch &addresses)
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
  co_return (co_await db_sched_client_->dbsched_write_hog_ages(ages)) ==
      DbSchedResult::DbSchedResultOK;
}

x23::Async<void>
NewScheduler::add_txs(const SignedTransactionBatch &txs)
{
  co_return;
}

void
NewScheduler::cycle_if_needed(const ContractAddress &account,
                              GasAmount gas_limit)
{}

void
NewScheduler::cycle_groups(const ContractAddress &account)
{}

void
NewScheduler::dump_runqueue() const
{}

x23::Async<void>
NewScheduler::process_received_hogs(const ContractAddress &account,
                                    uint128_t num_received,
                                    BlockHeight current_block)
{
  co_return;
}

x23::Async<void>
NewScheduler::process_deducted_hogs(const ContractAddress &account,
                                    uint128_t num_deducted)
{
  co_return;
}

x23::Async<
    std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>>>
NewScheduler::schedule(BlockHeight block_height, size_t block_size)
{
  std::pair<x23::Vec<SignedTransactionHash>, x23::Vec<SignedTransaction>> ret;
  co_return ret;
}

bool
NewScheduler::find_and_remove_from_runqueue(
    const x23::Vec<SignedTransactionHash> &hashes,
    x23::Vec<SignedTransaction> *ret)
{
  return false;
}

} // namespace hogd
