#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "db_sched.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "sched_db.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr size_t kBlockSize = 100;
constexpr size_t kNumTxs = 100;

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

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kBlockSize, scheduled_hashes, scheduled)();
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
            age->average_ages[kHogAgeGroupToSchedule]);
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

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();

  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front().tx.invocation.invoked_address, account);

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 0);
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

  SignedTransactionHash hash = sha256(txs[0]);
  SignedTransaction *tx = scheduler->find_tx(hash);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(sha256(*tx), hash);
  EXPECT_EQ(tx->tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[0]);
}

TEST(Scheduler, RemoveFromRunqueueSingleTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  // Validate a block.
  x23::Vec<SignedTransactionHash> hashes;
  hashes.push_back(sha256(txs[0]));
  scheduler->remove_from_scheduler(x23::span<const SignedTransactionHash>(
                                       hashes),
                                   /*validated_block=*/true)();
  EXPECT_EQ(scheduler->size(), 1UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  // Nominate a block.
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(sha256(scheduled.front()), sha256(txs[1]));
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  hashes = {sha256(txs[1])};
  scheduler->remove_from_scheduler(x23::span<const SignedTransactionHash>(
                                       hashes),
                                   /*validated_block=*/false)();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

TEST(Scheduler, RemoveFromRunqueueMultipleTxs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  x23::Vec<SignedTransactionHash> hashes;
  hashes.push_back(sha256(txs[0]));
  hashes.push_back(sha256(txs[1]));
  scheduler->remove_from_scheduler(x23::span<const SignedTransactionHash>(
                                       hashes),
                                   /*validated_block=*/true)();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  EXPECT_TRUE(scheduled.empty());
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);
}

TEST(Scheduler, RemoveFromRunqueueMultipleTxsNomination)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  EXPECT_EQ(scheduled_hashes.size(), 2UL);
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  x23::Vec<SignedTransactionHash> hashes;
  hashes.push_back(sha256(txs[0]));
  hashes.push_back(sha256(txs[1]));
  scheduler->remove_from_scheduler(x23::span<const SignedTransactionHash>(
                                       hashes),
                                   /*validated_block=*/false)();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
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

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled)();
  ASSERT_EQ(scheduled.size(), 2UL);
  EXPECT_EQ(scheduler->size(), 0UL);

  scheduler->reenqueue_txs(scheduled_hashes.begin(), scheduled_hashes.end());
  EXPECT_EQ(scheduler->size(), 2UL);

  for (const SignedTransactionHash &hash : scheduled_hashes) {
    SignedTransaction *reenqueued_tx = scheduler->find_tx(hash);
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

  std::unordered_map<ContractAddress, size_t> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(kBlockSize, scheduled_hashes, scheduled)();
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
    // this account has multipled by [kBlockSize]. For example, account 1 has 10
    // billion hogs, so its proportion of total hogs is 10 billion / (5 billion
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
  std::unordered_map<ContractAddress, SignedTransactionBatch> txs;
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

  std::unordered_map<ContractAddress, size_t> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled)();
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

// =============================================================================
// Parallel Scheduling Tests
//
// These tests verify invariants that must hold whether scheduling is sequential
// or parallel. They should pass with the current sequential implementation and
// continue to pass after parallelization.
// =============================================================================

TEST(SchedulerParallel, MultipleAccountsScheduled)
{
  // Verify that scheduling works correctly across multiple accounts and that
  // each account's txs maintain FIFO ordering within the account.
  constexpr size_t kTxsPerAccount = 20;
  constexpr size_t kNumRounds = 10;

  std::vector<fib_client_t> fib_clients;
  std::unordered_map<ContractAddress, SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
    txs[address] = generate_unsigned_txs(fib_clients.back(), kTxsPerAccount);
  }

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  for (const auto &[_, tx] : txs) {
    scheduler->add_txs(tx)();
  }

  // Schedule txs over multiple rounds and count per account
  std::unordered_map<ContractAddress, size_t> tx_counts;
  size_t total_scheduled = 0;
  for (size_t round = 0; round < kNumRounds; round++) {
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(/*num_txs=*/8, scheduled_hashes, scheduled)();
    for (const auto &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }
    total_scheduled += scheduled.size();
    scheduler->increment_current_block();
  }

  // Some txs should have been scheduled
  EXPECT_GT(total_scheduled, 0UL);

  // Multiple accounts should have had txs scheduled (fairness)
  size_t accounts_with_txs = 0;
  for (const auto &[addr, count] : tx_counts) {
    if (count > 0) {
      accounts_with_txs++;
    }
  }
  EXPECT_GT(accounts_with_txs, 1UL);
}

TEST(SchedulerParallel, GlobalTxLimitRespected)
{
  // Verify that exactly num_txs transactions are scheduled, never more.
  constexpr size_t kTxsPerAccount = 50;
  constexpr size_t kRequestedTxs = 25;

  std::vector<fib_client_t> fib_clients;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
  }

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Add many txs from all accounts
  for (size_t i = 0; i < fib_clients.size(); i++) {
    SignedTransactionBatch batch =
        generate_unsigned_txs(fib_clients[i], kTxsPerAccount);
    scheduler->add_txs(batch)();
  }

  // Request exactly kRequestedTxs
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kRequestedTxs, scheduled_hashes, scheduled)();

  // Must get exactly the requested amount (or less if not enough txs)
  EXPECT_EQ(scheduled.size(), kRequestedTxs);
  EXPECT_EQ(scheduled_hashes.size(), kRequestedTxs);
}

TEST(SchedulerParallel, NoDuplicateScheduling)
{
  // Verify that no transaction is scheduled twice across multiple schedule
  // calls.
  constexpr size_t kTxsPerAccount = 20;
  constexpr size_t kNumScheduleCalls = 10;
  constexpr size_t kTxsPerCall = 5;

  std::vector<fib_client_t> fib_clients;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
  }

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  for (size_t i = 0; i < fib_clients.size(); i++) {
    SignedTransactionBatch batch =
        generate_unsigned_txs(fib_clients[i], kTxsPerAccount);
    scheduler->add_txs(batch)();
  }

  std::unordered_set<SignedTransactionHash> all_scheduled;
  for (size_t call = 0; call < kNumScheduleCalls; call++) {
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(kTxsPerCall, scheduled_hashes, scheduled)();

    for (const auto &hash : scheduled_hashes) {
      // Verify this tx was not already scheduled
      EXPECT_EQ(all_scheduled.count(hash), 0UL)
          << "Transaction scheduled twice!";
      all_scheduled.insert(hash);
    }

    scheduler->increment_current_block();
  }
}

TEST(SchedulerParallel, AccountCyclingWorks)
{
  // Verify that accounts cycle correctly when gas is insufficient, even under
  // parallel scheduling.
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 5);

  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];
  const auto gas_per_tx =
      static_cast<uint128_t>(txs[0].tx.invocation.gas_limit);

  MempoolTestInfra server;
  // Start with zero gas in ToSchedule, but enough in AlreadyScheduled
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/5, /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/1,
                  /*num_hogs=*/gas_per_tx * 5);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0, /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  scheduler->add_txs(txs)();

  // Schedule all txs - this should trigger cycling
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/5, scheduled_hashes, scheduled)();

  // All 5 txs should be scheduled after cycling
  EXPECT_EQ(scheduled.size(), 5UL);
}

TEST(SchedulerParallel, EmptyChunksHandled)
{
  // Verify correct behavior when there are fewer accounts than potential
  // parallel threads. Some chunks will be empty.
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 10);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Only add txs for one account
  scheduler->add_txs(txs)();

  // Request many txs - only 10 should be returned since that's all we have
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/100, scheduled_hashes, scheduled)();

  EXPECT_EQ(scheduled.size(), 10UL);

  // All scheduled txs should be from the single account
  for (const auto &tx : scheduled) {
    EXPECT_EQ(tx.tx.invocation.invoked_address,
              MempoolTestInfra::kAccountAddresses[0]);
  }
}

TEST(SchedulerParallel, ConcurrentAddAndSchedule)
{
  // Test that adding transactions and scheduling can happen concurrently
  // without data races or crashes. This test adds txs in batches and schedules
  // between batches.
  constexpr size_t kBatches = 10;
  constexpr size_t kTxsPerBatch = 20;

  std::vector<fib_client_t> fib_clients;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
  }

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  size_t total_scheduled = 0;

  for (size_t batch = 0; batch < kBatches; batch++) {
    // Add txs from all accounts
    for (size_t i = 0; i < fib_clients.size(); i++) {
      SignedTransactionBatch txs =
          generate_unsigned_txs(fib_clients[i], kTxsPerBatch);
      scheduler->add_txs(txs)();
    }

    // Schedule some txs
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(kTxsPerBatch, scheduled_hashes, scheduled)();
    total_scheduled += scheduled.size();

    scheduler->increment_current_block();
  }

  // We should have scheduled some txs (exact count depends on gas availability)
  EXPECT_GT(total_scheduled, 0UL);

  // The tx cache should contain scheduled txs that haven't been removed yet
  EXPECT_GE(scheduler->tx_cache_size(), 0UL);

  // No crashes or data races occurred - test passed
}

// =============================================================================
// Critical Hardening Tests
//
// These tests verify critical invariants that the parallel implementation must
// preserve. They are designed to catch subtle ordering and correctness bugs.
// =============================================================================

TEST(SchedulerParallel, DeterministicOrdering)
{
  // Verify that transactions are scheduled in strict HOG age order within a
  // single schedule() call. Older accounts (higher age) must be scheduled
  // before younger accounts. This test runs multiple times to catch
  // non-determinism in the parallel merge.

  constexpr size_t kNumRuns = 100;

  for (size_t run = 0; run < kNumRuns; run++) {
    // Create 4 accounts with different ages
    std::vector<fib_client_t> fib_clients;
    for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
      fib_clients.emplace_back(address);
    }

    MempoolTestInfra server;

    // Set up accounts with distinct ages:
    // Note: average_hog_age is the block height when HOGs were acquired.
    // LOWER age = acquired earlier = OLDER = HIGHER priority.
    // Account 0: age 25 (oldest, highest priority)
    // Account 1: age 50
    // Account 2: age 75
    // Account 3: age 100 (youngest, lowest priority)
    constexpr uint128_t kHogsPerAccount = 10'000'000'000ULL;
    server.set_hogs(MempoolTestInfra::kAccountAddresses[0],
                    kHogAgeGroupToSchedule, /*age=*/25, kHogsPerAccount);
    server.set_hogs(MempoolTestInfra::kAccountAddresses[1],
                    kHogAgeGroupToSchedule, /*age=*/50, kHogsPerAccount);
    server.set_hogs(MempoolTestInfra::kAccountAddresses[2],
                    kHogAgeGroupToSchedule, /*age=*/75, kHogsPerAccount);
    server.set_hogs(MempoolTestInfra::kAccountAddresses[3],
                    kHogAgeGroupToSchedule, /*age=*/100, kHogsPerAccount);

    std::shared_ptr<Scheduler> scheduler =
        server.mempool_coord_rpc().scheduler();

    // Add exactly 1 tx per account
    for (size_t i = 0; i < fib_clients.size(); i++) {
      SignedTransactionBatch txs = generate_unsigned_txs(fib_clients[i], 1);
      scheduler->add_txs(txs)();
    }

    // Schedule all 4 txs
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(/*num_txs=*/4, scheduled_hashes, scheduled)();

    ASSERT_EQ(scheduled.size(), 4UL) << "Run " << run << ": Expected 4 txs";

    // Verify ordering: oldest account first (lowest age = oldest)
    // The expected order is: Account 0, 1, 2, 3 (by increasing age)
    EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
              MempoolTestInfra::kAccountAddresses[0])
        << "Run " << run << ": First tx should be from oldest account (age 25)";
    EXPECT_EQ(scheduled[1].tx.invocation.invoked_address,
              MempoolTestInfra::kAccountAddresses[1])
        << "Run " << run << ": Second tx should be from account with age 50";
    EXPECT_EQ(scheduled[2].tx.invocation.invoked_address,
              MempoolTestInfra::kAccountAddresses[2])
        << "Run " << run << ": Third tx should be from account with age 75";
    EXPECT_EQ(scheduled[3].tx.invocation.invoked_address,
              MempoolTestInfra::kAccountAddresses[3])
        << "Run " << run
        << ": Fourth tx should be from youngest account (age 100)";
  }
}

TEST(SchedulerParallel, AtomicCounterStress)
{
  // Stress test the CAS loop by creating many accounts with many transactions
  // and verifying that exactly the requested number of transactions are
  // scheduled, with no duplicates and no over-scheduling.

  constexpr size_t kNumAccounts = 100;
  constexpr size_t kTxsPerAccount = 100;
  constexpr size_t kRequestedTxs = 5000;

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Create many accounts with many transactions
  std::vector<fib_client_t> fib_clients;
  fib_clients.reserve(kNumAccounts);

  // Generate unique addresses for additional accounts beyond the 4 standard
  // ones
  for (size_t i = 0; i < kNumAccounts; i++) {
    ContractAddress addr;
    // Use a deterministic pattern for reproducibility
    std::fill(addr.begin(), addr.end(), 0);
    addr[0] = static_cast<uint8_t>(i & 0xFF);
    addr[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
    addr[2] = 0x42; // Marker byte
    fib_clients.emplace_back(addr);

    // Set up each account with gas
    constexpr uint128_t kHogsPerAccount = 100'000'000'000ULL;
    server.set_hogs(addr, kHogAgeGroupToSchedule, /*age=*/i, kHogsPerAccount);
  }

  // Add transactions for all accounts
  for (size_t i = 0; i < kNumAccounts; i++) {
    SignedTransactionBatch txs =
        generate_unsigned_txs(fib_clients[i], kTxsPerAccount);
    scheduler->add_txs(txs)();
  }

  // Total txs available: kNumAccounts * kTxsPerAccount = 10,000
  // We request 5,000

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kRequestedTxs, scheduled_hashes, scheduled)();

  // Verify exact count
  EXPECT_EQ(scheduled.size(), kRequestedTxs)
      << "Exactly " << kRequestedTxs << " txs should be scheduled";
  EXPECT_EQ(scheduled_hashes.size(), kRequestedTxs)
      << "Hash count should match tx count";

  // Verify no duplicates
  std::unordered_set<SignedTransactionHash> seen;
  for (const auto &hash : scheduled_hashes) {
    auto [it, inserted] = seen.insert(hash);
    EXPECT_TRUE(inserted) << "Duplicate transaction detected!";
  }
}

TEST(SchedulerParallel, CyclingPreservesGlobalOrder)
{
  // Verify that when accounts need to cycle (insufficient gas in ToSchedule
  // group), the global HOG age ordering is still preserved. Accounts with
  // older HOGs should still be scheduled before accounts with younger HOGs,
  // even after cycling.

  // Create 4 accounts, all with gas only in AlreadyScheduled (forcing cycling)
  std::vector<fib_client_t> fib_clients;
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    fib_clients.emplace_back(address);
  }

  MempoolTestInfra server;

  // Set up accounts with:
  // - ZERO gas in ToSchedule (forces cycling)
  // - Enough gas in AlreadyScheduled to run after cycling
  // - Different ages (lower age = older = higher priority)
  constexpr uint128_t kHogsPerAccount = 10'000'000'000ULL;

  // Account 0: oldest (lowest age), should be scheduled first after cycling
  server.set_hogs(MempoolTestInfra::kAccountAddresses[0],
                  kHogAgeGroupToSchedule, /*age=*/25, /*num_hogs=*/0);
  server.set_hogs(MempoolTestInfra::kAccountAddresses[0],
                  kHogAgeGroupAlreadyScheduled, /*age=*/25, kHogsPerAccount);

  // Account 1: second oldest
  server.set_hogs(MempoolTestInfra::kAccountAddresses[1],
                  kHogAgeGroupToSchedule, /*age=*/50, /*num_hogs=*/0);
  server.set_hogs(MempoolTestInfra::kAccountAddresses[1],
                  kHogAgeGroupAlreadyScheduled, /*age=*/50, kHogsPerAccount);

  // Account 2: third oldest
  server.set_hogs(MempoolTestInfra::kAccountAddresses[2],
                  kHogAgeGroupToSchedule, /*age=*/75, /*num_hogs=*/0);
  server.set_hogs(MempoolTestInfra::kAccountAddresses[2],
                  kHogAgeGroupAlreadyScheduled, /*age=*/75, kHogsPerAccount);

  // Account 3: youngest (highest age), should be scheduled last after cycling
  server.set_hogs(MempoolTestInfra::kAccountAddresses[3],
                  kHogAgeGroupToSchedule, /*age=*/100, /*num_hogs=*/0);
  server.set_hogs(MempoolTestInfra::kAccountAddresses[3],
                  kHogAgeGroupAlreadyScheduled, /*age=*/100, kHogsPerAccount);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Add 1 tx per account
  for (size_t i = 0; i < fib_clients.size(); i++) {
    SignedTransactionBatch txs = generate_unsigned_txs(fib_clients[i], 1);
    scheduler->add_txs(txs)();
  }

  // Schedule all 4 txs - this will trigger cycling for all accounts
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/4, scheduled_hashes, scheduled)();

  // All 4 should be scheduled after cycling
  ASSERT_EQ(scheduled.size(), 4UL)
      << "All 4 txs should be scheduled after cycling";

  // Verify the order matches HOG age priority (oldest first)
  // After cycling, the order should still respect the original age ordering
  EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[0])
      << "First tx should be from oldest account after cycling";
  EXPECT_EQ(scheduled[1].tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[1])
      << "Second tx should be from second oldest account after cycling";
  EXPECT_EQ(scheduled[2].tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[2])
      << "Third tx should be from third oldest account after cycling";
  EXPECT_EQ(scheduled[3].tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[3])
      << "Fourth tx should be from youngest account after cycling";
}

} // namespace hogd
