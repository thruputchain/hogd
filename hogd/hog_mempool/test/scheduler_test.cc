#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/scheduler.h"
#include "hogd/hog_mempool/test/infra.h"
#include "hogd/hog_mempool/test/txs.h"
#include "sched_db.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr size_t kBlockSize = 100;
constexpr size_t kNumTxs = 100;

// Polls [pred] until it holds or [timeout] elapses.
template<typename Pred>
bool
wait_until(Pred pred,
           std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Whether some other thread currently holds [mutex]. [try_lock()] may fail
// spuriously, so a single failure is not taken as proof.
bool
mutex_is_held(std::mutex &mutex)
{
  for (int i = 0; i < 3; i++) {
    if (mutex.try_lock()) {
      mutex.unlock();
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

} // namespace

TEST(Scheduler, ReadAge)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->read_average_hog_ages({MempoolTestInfra::kAccountAddresses[0]})();
  const auto [ret, lock] =
      scheduler->get_cached_hog_age(MempoolTestInfra::kAccountAddresses[0]);
  EXPECT_EQ(ret->num_hogs[kHogAgeGroupToSchedule],
            // TODO(jhumphri): Update this, too.
            to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)));
  EXPECT_EQ(ret->average_ages[kHogAgeGroupToSchedule], 0);
}

TEST(Scheduler, AddVettedTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  EXPECT_EQ(scheduler->size(), 0UL);
  scheduler->add_txs({txs[0]})();
  EXPECT_EQ(scheduler->size(), 1UL);
}

TEST(Scheduler, AddVettedTxFromUnknownAccount)
{
  const ContractAddress kUnknownAccount = make_account_address(99);

  fib_client_t fib_client(kUnknownAccount);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  scheduler->add_txs({txs[0]})();

  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
  EXPECT_EQ(scheduler->find_tx(sha256(txs[0].tx)), nullptr);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_TRUE(scheduled.empty());
}

TEST(Scheduler, LocalSchedulingDropsTxWhenAllHogAgeGroupsAreEmpty)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0, /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs({txs[0]})();
  EXPECT_EQ(scheduler->size(), 1UL);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);

  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, AddVettedTxDuplicate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  EXPECT_EQ(scheduler->size(), 0UL);
  scheduler->add_txs({txs[0]})();
  EXPECT_EQ(scheduler->size(), 1UL);
  // Now add the transaction a second time.
  scheduler->add_txs({txs[0]})();
  EXPECT_EQ(scheduler->size(), 1UL);
}

TEST(Scheduler, CycleGroupsMovesHogsBetweenBuckets)
{
  HogAge hog_age{};
  hog_age.address = MempoolTestInfra::kAccountAddresses[0];
  hog_age.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<uint128_t>(1));
  hog_age.average_ages[kHogAgeGroupToSchedule] = 10;
  hog_age.num_hogs[kHogAgeGroupAlreadyScheduled] =
      to_xdr(static_cast<uint128_t>(2));
  hog_age.average_ages[kHogAgeGroupAlreadyScheduled] = 5;
  hog_age.num_hogs[kHogAgeGroupReceived] = to_xdr(static_cast<uint128_t>(3));
  hog_age.average_ages[kHogAgeGroupReceived] = 1;

  Scheduler::cycle_groups(hog_age);

  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(3));
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupToSchedule], 7);
  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(3));
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupAlreadyScheduled], 1);
  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, Single)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]})();
  // Check that the correct number of hogs was deducted.
  auto [age, lock] =
      scheduler->get_cached_hog_age(MempoolTestInfra::kAccountAddresses[0]);
  lock.unlock();

  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
            5'000'000'000 * 10);
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 0);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kBlockSize, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 1);
  EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[0]);

  // Check the correct number of hogs was deducted from the
  // [kHogAgeGroupToSchedule] group.
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 9);
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 0);

  // Check that the correct number of hogs was added to the
  // [kHogAgeGroupAlreadyScheduled] group.
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            5'000'000'000);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled],
            age->average_ages[kHogAgeGroupToSchedule] + 2);
}

TEST(Scheduler, PendingNominationKeepsLiveHogAgesUnchangedUntilCommit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]})();

  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  auto [before_commit, before_lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(before_commit->num_hogs[kHogAgeGroupToSchedule]),
            5'000'000'000 * 10);
  EXPECT_EQ(from_xdr(before_commit->num_hogs[kHogAgeGroupAlreadyScheduled]),
            0UL);
  before_lock.unlock();

  scheduler->start_pending_nomination(/*block_id=*/1);
  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 1UL);

  auto [during_nomination, during_lock] =
      scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(during_nomination->num_hogs[kHogAgeGroupToSchedule]),
            5'000'000'000 * 10);
  EXPECT_EQ(from_xdr(during_nomination->num_hogs[kHogAgeGroupAlreadyScheduled]),
            0UL);
  during_lock.unlock();

  scheduler->commit_pending_nomination(/*block_id=*/1);

  auto [after_commit, after_lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(after_commit->num_hogs[kHogAgeGroupToSchedule]),
            (5'000'000'000 * 10) - gas_limit);
  EXPECT_EQ(from_xdr(after_commit->num_hogs[kHogAgeGroupAlreadyScheduled]),
            gas_limit);
  EXPECT_EQ(after_commit->average_ages[kHogAgeGroupAlreadyScheduled], 2);
  after_lock.unlock();
}

TEST(Scheduler, PendingNominationExcludesUnschedulableAccountAcrossSchedules)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  txs[0].tx.uniquifier.block_expiry_height = 10;
  txs[1].tx.uniquifier.block_expiry_height = 11;
  txs[1].tx.invocation.gas_limit = txs[0].tx.invocation.gas_limit * 2;
  txs[2].tx.uniquifier.block_expiry_height = 12;

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1], txs[2]})();
  EXPECT_EQ(scheduler->size(), 3UL);

  scheduler->start_pending_nomination(/*block_id=*/1);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[0]);
  EXPECT_EQ(scheduler->size(), 1UL);

  scheduled_hashes.clear();
  scheduled.clear();
  // Check that no tx is scheduled.
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 1UL);

  scheduler->commit_pending_nomination(/*block_id=*/1);

  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[2]);
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, AddTxsDoesNotReenableAccountExcludedByPendingNomination)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  txs[0].tx.uniquifier.block_expiry_height = 10;
  txs[1].tx.uniquifier.block_expiry_height = 11;
  txs[1].tx.invocation.gas_limit = txs[0].tx.invocation.gas_limit * 2;
  txs[2].tx.uniquifier.block_expiry_height = 12;

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);

  scheduler->start_pending_nomination(/*block_id=*/1);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[0]);
  EXPECT_EQ(scheduler->size(), 0UL);

  scheduler->add_txs({txs[2]})();
  EXPECT_EQ(scheduler->size(), 1UL);

  scheduled_hashes.clear();
  scheduled.clear();
  // Check that no tx is scheduled.
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 1UL);

  scheduler->abandon_pending_nomination(/*block_id=*/1);

  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[2]);
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, CommitTouchingExcludedAccountRemovesFromScheduler)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);
  const auto balance = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);
  txs[0].tx.uniquifier.block_expiry_height = 10; // affordable
  txs[1].tx.uniquifier.block_expiry_height = 11;
  txs[1].tx.invocation.gas_limit =
      txs[0].tx.invocation.gas_limit * 2;        // > balance
  txs[2].tx.uniquifier.block_expiry_height = 12; // ordinary follow-up

  MempoolTestInfra server;
  server.mempool_coord_rpc().wait_for_block_commit(/*expected_height=*/1)();

  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/balance);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);

  scheduler->start_pending_nomination(/*block_id=*/1);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
  // txs[0] is nominated; txs[1] is over budget, so [account] is excluded from
  // the block and dropped from [ready_accounts_] for the rest of the block.
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front(), txs[0]);
  EXPECT_EQ(scheduler->size(), 0UL);

  // An ordinary follow-up submission lands on the excluded account's runqueue,
  // but [add_txs] refuses to re-insert the account into [ready_accounts_].
  scheduler->add_txs({txs[2]})();
  EXPECT_EQ(scheduler->size(), 1UL);
  ASSERT_NE(scheduler->find_tx(sha256(txs[2].tx)), nullptr);

  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  EXPECT_TRUE(scheduled.empty()); // still excluded

  // Commit a block that touches the excluded account's runqueue tx.
  // In [remove_from_scheduler()], [account_is_ready()] is false and
  // [ready_accounts_.erase(...)] returns 0. Before the fix,
  // [ready_accounts_.erase(...)] would trigger a CHECK.
  x23::Vec<TransactionHash> hashes;
  hashes.push_back(sha256(txs[2].tx));
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();

  // We only reach this point if the fix is working.
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->find_tx(sha256(txs[2].tx)), nullptr);
}

TEST(Scheduler, ProcessTransfersUpdatesReceivedGroup)
{
  constexpr BlockHeight kBlock = 5;
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  HogsTransferred transfer;
  transfer.block = kBlock;
  transfer.deltas.push_back(
      Delta{.address = account,
            .hog_delta = to_xdr(static_cast<int128_t>(5'000'000'000))});

  scheduler->process_transfers(transfer)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(5'000'000'000));
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], kBlock);
}

TEST(Scheduler, ProcessTransfersCreatesHogAgeForFirstTimeRecipient)
{
  constexpr BlockHeight kBlock = 5;
  const ContractAddress account = make_account_address(99);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  HogsTransferred transfer;
  transfer.block = kBlock;
  transfer.deltas.push_back(
      Delta{.address = account,
            .hog_delta = to_xdr(static_cast<int128_t>(5'000'000'000))});

  scheduler->process_transfers(transfer)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(age->address, account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(5'000'000'000));
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], kBlock);
}

TEST(Scheduler, ApplyCommittedBlockHogAgesUpdatesLiveState)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/5, /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/1,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->apply_committed_block_hog_ages({txs[0]}, /*block_height=*/1)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]), gas_limit);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 1);
}

TEST(Scheduler, ApplyCommittedBlockHogAgesSkipsAccountWhenGasExceedsLocalHogs)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0, /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs({txs[0]})();
  scheduler->apply_committed_block_hog_ages({txs[0]}, /*block_height=*/1)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, ApplyCommittedBlockHogAgesSkipsOnlyInvalidAccounts)
{
  const ContractAddress &valid_account = MempoolTestInfra::kAccountAddresses[0];
  const ContractAddress &invalid_account =
      MempoolTestInfra::kAccountAddresses[1];
  fib_client_t valid_client(valid_account);
  fib_client_t invalid_client(invalid_account);
  const SignedTransactionBatch valid_txs =
      generate_unsigned_txs(valid_client, 1);
  const SignedTransactionBatch invalid_txs =
      generate_unsigned_txs(invalid_client, 1);
  const auto gas_limit =
      static_cast<uint128_t>(valid_txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  server.set_hogs(valid_account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(valid_account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(valid_account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(invalid_account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(invalid_account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(invalid_account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs({valid_txs[0], invalid_txs[0]})();
  scheduler->apply_committed_block_hog_ages({valid_txs[0], invalid_txs[0]},
                                            /*block_height=*/1)();

  {
    const auto [age, lock] = scheduler->get_cached_hog_age(valid_account);
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]), gas_limit);
  }
  {
    const auto [age, lock] = scheduler->get_cached_hog_age(invalid_account);
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
              static_cast<uint128_t>(0));
    EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
              static_cast<uint128_t>(0));
  }
}

TEST(Scheduler, ApplyCommittedBlockHogAgesRollsBackAccountWhenLaterTxFails)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  fib_client_t fib_client(account);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);
  txs[1].tx.invocation.gas_limit = txs[0].tx.invocation.gas_limit * 2;
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs({txs[0], txs[1]})();
  scheduler->apply_committed_block_hog_ages({txs[0], txs[1]},
                                            /*block_height=*/1)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), gas_limit);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, ProcessTransfersHandlesWithdrawalsAcrossGroups)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  constexpr auto kGas = static_cast<uint128_t>(5'000'000'000);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0, kGas * 4);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0, kGas * 3);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, kGas * 2);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  HogsTransferred transfer;
  transfer.block = 10;
  const auto signed_gas = static_cast<int128_t>(kGas);
  const int128_t delta_value = -8 * signed_gas;
  transfer.deltas.push_back(
      Delta{.address = account, .hog_delta = to_xdr(delta_value)});

  scheduler->process_transfers(transfer)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), kGas);
}

TEST(Scheduler, CycleAccountWhenGasInsufficient)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  const auto gas_limit = static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/5,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/1,
                  /*num_hogs=*/gas_limit);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]})();

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);

  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front().tx.invocation.invoked_address, account);

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 2);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(txs[0].tx.invocation.gas_limit));
}

TEST(Scheduler, FindTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]})();
  EXPECT_EQ(scheduler->size(), 1UL);

  TransactionHash hash = sha256(txs[0].tx);
  std::shared_ptr<const SignedTransaction> tx = scheduler->find_tx(hash);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(sha256(tx->tx), hash);
  EXPECT_EQ(tx->tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[0]);
}

TEST(Scheduler, RemoveFromRunqueueSingleTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  // Genesis initialization and the ceremony trigger [auth_data_changed()]
  // callbacks in the mempool, which call [remove_from_scheduler()]. We call
  // the same method below, so wait for both callbacks to complete.
  server.mempool_coord_rpc().wait_for_block_commit(/*expected_height=*/1)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  // Validate a block.
  x23::Vec<TransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  EXPECT_EQ(scheduler->size(), 1UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  // Nominate a block.
  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(sha256(scheduled.front().tx), sha256(txs[1].tx));
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  hashes = {sha256(txs[1].tx)};
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

TEST(Scheduler, RemoveFromRunqueueMultipleTxs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  // Genesis initialization and the ceremony trigger [auth_data_changed()]
  // callbacks in the mempool, which call [remove_from_scheduler()]. We call
  // the same method below, so wait for both callbacks to complete.
  server.mempool_coord_rpc().wait_for_block_commit(/*expected_height=*/1)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  x23::Vec<TransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  hashes.push_back(sha256(txs[1].tx));
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

TEST(Scheduler, RemoveFromRunqueueMultipleTxsNomination)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  // Genesis initialization and the ceremony trigger [auth_data_changed()]
  // callbacks in the mempool, which call [remove_from_scheduler()]. We call
  // the same method below, so wait for both callbacks to complete.
  server.mempool_coord_rpc().wait_for_block_commit(/*expected_height=*/1)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);
  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
  EXPECT_EQ(scheduled_hashes.size(), 2UL);
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  x23::Vec<TransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  hashes.push_back(sha256(txs[1].tx));
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

TEST(Scheduler, ReenqueueTxs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);

  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 2UL);
  EXPECT_EQ(scheduler->size(), 0UL);

  scheduler->reenqueue_txs(scheduled_hashes.begin(), scheduled_hashes.end());
  EXPECT_EQ(scheduler->size(), 2UL);

  for (const TransactionHash &hash : scheduled_hashes) {
    std::shared_ptr<const SignedTransaction> reenqueued_tx =
        scheduler->find_tx(hash);
    ASSERT_NE(reenqueued_tx, nullptr);
  }
}

TEST(Scheduler, Fairness)
{
  constexpr size_t kNumRounds = 100;

  std::vector<fib_client_t> fib_clients;
  std::vector<SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs.push_back(
        generate_unsigned_txs(fib_clients.back(), kBlockSize * kNumRounds));
  }

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and hogs:
  //
  // Account 0: Age 3 and 5 billion hogs.
  // Account 1: Age 2 and 10 billion hogs.
  // Account 2: Age 1 and 15 billion hogs.
  // Account 3: Age 0 and 20 billion hogs.
  //
  // Thus, there are 50 billion hogs in total across the four accounts.
  for (size_t i = 0; i < MempoolTestInfra::kAccountAddresses.size(); i++) {
    server.set_hogs(MempoolTestInfra::kAccountAddresses[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/MempoolTestInfra::kAccountAddresses.size() - i - 1,
                    /*num_hogs=*/5'000'000'000 * (i + 1));
  }
  for (const SignedTransactionBatch &tx : txs) {
    scheduler->add_txs(tx)();
  }

  std::unordered_map<ContractAddress, size_t, SipHash24Hasher> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    x23::XdrVec<TransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(kBlockSize, scheduled_hashes, scheduled);
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }

    // We simulate committing the block by increment the current block number in
    // the scheduler.
    scheduler->increment_current_block();
  }

  for (size_t i = 0; i < MempoolTestInfra::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestInfra::kAccountAddresses[i];
    // The number of txs scheduled should be the proportion of total hogs that
    // this account has multiplied by [kBlockSize]. For example, account 1 has
    // 10 billion hogs, so its proportion of total hogs is 10 billion / (5
    // billion
    // + 10 billion + 15 billion + 20 billion) = 10 billion / 50 billion = 0.2.
    // Thus, the number of txs scheduled for account 1 should be 0.2 *
    // [kBlockSize] * [kNumRounds].
    //
    // We divide by 10 at the end rather than do (i + 1) / 10 * [kBlockSize] *
    // [kNumRounds] to avoid truncation due to integer division.
    const size_t target = (i + 1) * kBlockSize * kNumRounds / 10;
    EXPECT_LE(tx_counts[address],
              target + MempoolTestInfra::kAccountAddresses.size());
    EXPECT_GE(tx_counts[address],
              target - MempoolTestInfra::kAccountAddresses.size());
  }
}

TEST(Scheduler, FairnessAcrossBlocks)
{
  constexpr size_t kNumRounds = 100;
  static_assert(kNumRounds % 10 == 0);

  std::vector<fib_client_t> fib_clients;
  std::unordered_map<ContractAddress, SignedTransactionBatch, SipHash24Hasher>
      txs;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs[address] =
        generate_unsigned_txs(fib_clients.back(), kBlockSize * kNumRounds);
  }

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and
  // hogs:
  //
  // Account 0: Age 0 and 5 billion hogs.  (Will get 10% of the throughput.)
  // Account 1: Age 0 and 10 billion hogs. (Will get 20% of the throughput.)
  // Account 2: Age 0 and 15 billion hogs. (Will get 30% of the throughput.)
  // Account 3: Age 0 and 20 billion hogs. (Will get 40% of the throughput.)
  //
  // Thus, there are 50 billion hogs in total across the four accounts.
  for (size_t i = 0; i < MempoolTestInfra::kAccountAddresses.size(); i++) {
    server.set_hogs(MempoolTestInfra::kAccountAddresses[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/0,
                    /*num_hogs=*/5'000'000'000 * (i + 1));
  }
  for (const auto &[_, tx] : txs) {
    scheduler->add_txs(tx)();
  }

  std::unordered_map<ContractAddress, size_t, SipHash24Hasher> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    x23::XdrVec<TransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
    EXPECT_EQ(scheduled.size(), 1UL);
    tx_counts[scheduled.front().tx.invocation.invoked_address]++;

    // We simulate committing the block by increment the current block number in
    // the scheduler.
    scheduler->increment_current_block();
  }

  for (size_t i = 0; i < MempoolTestInfra::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestInfra::kAccountAddresses[i];
    const size_t target = (i + 1) * (kNumRounds / 10);
    EXPECT_LE(tx_counts[address],
              target + MempoolTestInfra::kAccountAddresses.size());
    EXPECT_GE(tx_counts[address],
              target - MempoolTestInfra::kAccountAddresses.size());
  }
}

TEST(Scheduler, UnschedulableHeadTxCrashesScheduler)
{
  const ContractAddress &account_a = MempoolTestInfra::kAccountAddresses[0];
  const ContractAddress &account_b = MempoolTestInfra::kAccountAddresses[1];

  fib_client_t fib_client_a(account_a);
  fib_client_t fib_client_b(account_b);

  MempoolTestInfra server;
  server.set_hogs(account_a, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/100);
  server.set_hogs(account_a, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account_a, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  constexpr char8_t kProgramName[] = u8"slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  SignedTransaction tx1{
      .tx = Transaction{
          .invocation = fib_client_a.make_invocation<kProgramName>(
              /*gas_amount=*/200, kFibonacciN),
          .uniquifier = {.block_expiry_height = 1000, .nonce = 0},
          .contracts_to_deploy = {},
      }};

  SignedTransaction tx2{
      .tx = Transaction{
          .invocation = fib_client_a.make_invocation<kProgramName>(
              /*gas_amount=*/50, kFibonacciN),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 1},
          .contracts_to_deploy = {},
      }};

  SignedTransaction tx_b{
      .tx = Transaction{
          .invocation = fib_client_b.make_invocation<kProgramName>(
              /*gas_amount=*/5'000'000'000, kFibonacciN),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 0},
          .contracts_to_deploy = {},
      }};

  scheduler->add_txs({tx1, tx2, tx_b})();

  {
    x23::XdrVec<TransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    // Ensure that this does not trigger a CHECK in the scheduler (see issue
    // #1474).
    scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  }
}

// [add_txs_helper()] looks the tx up under the cache bucket lock, drops that
// lock, and only then takes the account lock to insert the tx into the
// runqueue. [schedule()]'s expiry sweep nests the other way: it holds
// the account lock while it removes expired txs from the cache. So a resubmit
// of an expired tx that finds it in the cache just as the sweep is removing it
// could re-insert the tx into the runqueue with no cache entry behind it
// before the fix added a cache recheck under the account lock. The next sweep
// of that account would fail [CHECK_EQ(txs_.remove(tx_hash), 1UL)].
//
// The test stages that interleaving with two txs on one account, both expired.
// [blocker] sorts first in the runqueue; holding its cache bucket lock parks
// the sweep after it has taken the account lock but before it reaches [victim].
// Holding [victim]'s cache bucket lock parks the resubmit inside
// [add_txs_helper()] before it can take the account lock.
TEST(Scheduler, ResubmitDuringExpirySweepKeepsRunqueueAndCacheConsistent)
{
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  const ContractAddress &other_account = MempoolTestInfra::kAccountAddresses[1];
  fib_client_t fib_client(account);
  fib_client_t other_fib_client(other_account);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  constexpr char8_t kProgramName[] = u8"slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;
  const BlockHeight start_block = scheduler->current_block();

  auto make_tx = [&](fib_client_t &client, BlockHeight expiry, uint64_t nonce) {
    return SignedTransaction{
        .tx = Transaction{
            .invocation = client.make_invocation<kProgramName>(
                /*gas_amount=*/200, kFibonacciN),
            .uniquifier = {.block_expiry_height = expiry, .nonce = nonce},
            .contracts_to_deploy = {},
        }};
  };
  // Makes a tx from [client] whose cache bucket lock is none of [taken].
  auto make_tx_outside_buckets =
      [&](fib_client_t &client, BlockHeight expiry,
          std::initializer_list<const std::mutex *> taken) {
        constexpr uint64_t kMaxNonce = 64;
        for (uint64_t nonce = 1;; nonce++) {
          CHECK_LE(nonce, kMaxNonce);
          SignedTransaction candidate = make_tx(client, expiry, nonce);
          const TransactionHash hash = sha256(candidate.tx);
          const std::mutex *bucket = &scheduler->tx_cache_bucket_mutex(hash);
          if (std::ranges::find(taken, bucket) == taken.end()) {
            return std::pair{std::move(candidate), hash};
          }
        }
      };

  // [blocker] has the lower expiry height, so the sweep removes it before
  // [victim]. The two need distinct cache buckets so that each can be pinned
  // on its own.
  const SignedTransaction blocker = make_tx(fib_client, start_block + 1,
                                            /*nonce=*/0);
  const TransactionHash blocker_hash = sha256(blocker.tx);
  std::mutex &blocker_bucket = scheduler->tx_cache_bucket_mutex(blocker_hash);
  const auto [victim, victim_hash] =
      make_tx_outside_buckets(fib_client, start_block + 2, {&blocker_bucket});
  std::mutex &victim_bucket = scheduler->tx_cache_bucket_mutex(victim_hash);
  // [probe] rides along in the resubmit batch ahead of [victim]. It targets an
  // account the scheduler has not cached yet, so [add_txs()] must finish its
  // hog age refresh (and release [global_lock_]) before [add_txs_helper()]
  // inserts [probe] into the cache; seeing [probe] via [find_tx()] therefore
  // tells the test the resubmit has reached the pinned lookup of [victim].
  // [probe] must not share a bucket with either pin or it would park too.
  const auto [probe, probe_hash] =
      make_tx_outside_buckets(other_fib_client, start_block + 2,
                              {&blocker_bucket, &victim_bucket});

  scheduler->add_txs({blocker, victim})();
  ASSERT_EQ(scheduler->size(), 2);
  ASSERT_EQ(scheduler->tx_cache_size(), 2UL);
  ASSERT_NE(scheduler->find_tx(victim_hash), nullptr);
  // Move the chain past every expiry height so the next sweep drops them all.
  scheduler->set_current_block(start_block + 3);

  std::unique_lock<std::mutex> pin_resubmit(victim_bucket);
  std::unique_lock<std::mutex> pin_sweep(blocker_bucket);

  // Resubmit the identical bytes of the expired [victim], as the gateway path
  // does. The resubmit caches [probe], then parks on [victim_bucket] inside
  // [add_txs_helper()].
  std::thread resubmit([&] { scheduler->add_txs({probe, victim})(); });
  const bool resubmit_parked =
      wait_until([&] { return scheduler->find_tx(probe_hash) != nullptr; });

  // Start the nomination sweep. It takes the account lock, finds both txs
  // expired, and parks on [blocker_bucket] while still holding the account
  // lock.
  x23::XdrVec<TransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  // The accessor itself takes the mutex, so obtain it before the sweep can
  // hold it while waiting for [pin_sweep] to be released by this thread.
  std::mutex &account_mutex = scheduler->account_bucket_mutex(account);
  std::thread sweep([&] {
    [](Scheduler &scheduler, x23::XdrVec<TransactionHash> &hashes,
       x23::XdrVec<SignedTransaction> &txs) -> x23::Async<void> {
      x23::CoroGuard cg;
      co_await cg(scheduler.global_lock_);
      scheduler.schedule(kBlockSize, hashes, txs);
    }(*scheduler, scheduled_hashes, scheduled)();
  });
  const bool sweep_parked = resubmit_parked && wait_until([&] {
                              return mutex_is_held(account_mutex);
                            });

  // Release the resubmit first: it finds [victim] already cached, drops the
  // cache lock, and blocks on the account lock the sweep holds. There is no
  // observable signal for the resubmit reaching that wait, so give it time.
  // If the wait is too short the sweep simply runs first and the test passes
  // without exercising the race; it never fails for a false reason.
  pin_resubmit.unlock();
  if (sweep_parked) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  // Now let the sweep finish. It removes both txs from the cache and the
  // runqueue and releases the account lock; the resubmit then inserts [victim]
  // into the runqueue.
  pin_sweep.unlock();
  sweep.join();
  resubmit.join();

  ASSERT_TRUE(resubmit_parked) << "resubmit never cached [probe]";
  ASSERT_TRUE(sweep_parked) << "schedule() never took the account lock";
  EXPECT_TRUE(scheduled_hashes.empty());

  // Every runqueue entry must be backed by a cache entry.
  TransactionHashBatch runqueue_hashes;
  SignedTransactionBatch runqueue_txs;
  scheduler->get_txs_for_account(account, runqueue_hashes, runqueue_txs);
  scheduler->get_txs_for_account(other_account, runqueue_hashes, runqueue_txs);
  for (const TransactionHash &tx_hash : runqueue_hashes) {
    EXPECT_NE(scheduler->find_tx(tx_hash), nullptr)
        << "tx is on the account runqueue but missing from the tx cache; the "
           "next schedule() of this account would fail "
           "CHECK_EQ(txs_.remove(tx_hash), 1UL)";
  }
  EXPECT_EQ(static_cast<size_t>(scheduler->size()), runqueue_hashes.size());
  EXPECT_EQ(scheduler->tx_cache_size(), runqueue_hashes.size());
  if (HasFailure()) {
    // The next sweep would abort on the orphaned runqueue entry.
    return;
  }

  // A further sweep drains whatever the resubmit left behind.
  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(kBlockSize, scheduled_hashes, scheduled);
  EXPECT_TRUE(scheduled_hashes.empty());
  EXPECT_EQ(scheduler->size(), 0);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

} // namespace hogd
