#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
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
            age->average_ages[kHogAgeGroupToSchedule] + 1);
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
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);

  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front().tx.invocation.invoked_address, account);

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 1);
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

  SignedTransactionHash hash = sha256(txs[0].tx);
  SignedTransaction *tx = scheduler->find_tx(hash);
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
  // The genesis block triggers the [auth_data_changed()] callback in the
  // mempool, which calls [remove_from_scheduler()]. We call the same method
  // below, so to avoid a data race, we wait for the [auth_data_changed()]
  // callback to complete.
  server.mempool_coord_rpc().wait_for_auth_data(/*expected_height=*/0)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  // Validate a block.
  x23::Vec<SignedTransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  scheduler->remove_from_scheduler(x23::span<const SignedTransactionHash>(
                                       hashes),
                                   /*validated_block=*/true)();
  EXPECT_EQ(scheduler->size(), 1UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  // Nominate a block.
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(sha256(scheduled.front().tx), sha256(txs[1].tx));
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 1UL);

  hashes = {sha256(txs[1].tx)};
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
  // The genesis block triggers the [auth_data_changed()] callback in the
  // mempool, which calls [remove_from_scheduler()]. We call the same method
  // below, so to avoid a data race, we wait for the [auth_data_changed()]
  // callback to complete.
  server.mempool_coord_rpc().wait_for_auth_data(/*expected_height=*/0)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  x23::Vec<SignedTransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  hashes.push_back(sha256(txs[1].tx));
  scheduler->remove_from_scheduler(x23::span<const SignedTransactionHash>(
                                       hashes),
                                   /*validated_block=*/true)();
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 0UL);

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
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
  // The genesis block triggers the [auth_data_changed()] callback in the
  // mempool, which calls [remove_from_scheduler()]. We call the same method
  // below, so to avoid a data race, we wait for the [auth_data_changed()]
  // callback to complete.
  server.mempool_coord_rpc().wait_for_auth_data(/*expected_height=*/0)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
  EXPECT_EQ(scheduled_hashes.size(), 2UL);
  EXPECT_EQ(scheduler->size(), 0UL);
  EXPECT_EQ(scheduler->tx_cache_size(), 2UL);

  x23::Vec<SignedTransactionHash> hashes;
  hashes.push_back(sha256(txs[0].tx));
  hashes.push_back(sha256(txs[1].tx));
  scheduler->remove_from_scheduler(x23::span<const SignedTransactionHash>(
                                       hashes),
                                   /*validated_block=*/false)();
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

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
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

TEST(Scheduler, BugHunt_CycleGroupsAllEmpty)
{
  // Hypothesis: When all three groups have 0 hogs, cycle_groups should produce
  // all-zero output. But the else-branch at line 326 of scheduler.cc sets
  // average_ages[kHogAgeGroupToSchedule] = already_scheduled_age, even when
  // both to_schedule and already_scheduled have 0 hogs. If
  // already_scheduled_age is nonzero, the to_schedule group gets a nonzero age
  // despite having 0 hogs.
  HogAge hog_age{};
  hog_age.address = MempoolTestInfra::kAccountAddresses[0];
  // All groups have 0 hogs, but already_scheduled has a nonzero age.
  hog_age.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<uint128_t>(0));
  hog_age.average_ages[kHogAgeGroupToSchedule] = 0;
  hog_age.num_hogs[kHogAgeGroupAlreadyScheduled] =
      to_xdr(static_cast<uint128_t>(0));
  hog_age.average_ages[kHogAgeGroupAlreadyScheduled] = 42;
  hog_age.num_hogs[kHogAgeGroupReceived] = to_xdr(static_cast<uint128_t>(0));
  hog_age.average_ages[kHogAgeGroupReceived] = 0;

  Scheduler::cycle_groups(hog_age);

  // After cycling, to_schedule should have 0 hogs. The age should be 0 since
  // there are no hogs, but the code sets it to already_scheduled_age (42).
  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  // This EXPECT will FAIL if the bug exists: age will be 42, not 0.
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupToSchedule], 0)
      << "to_schedule age should be 0 when there are 0 hogs, but got stale "
         "age from already_scheduled group";
}

TEST(Scheduler, BugHunt_ScheduleExpiredTxsAreRemoved)
{
  // Hypothesis: Txs with expiry_block_height <= current_block should be
  // removed during schedule(). If the current block is advanced past the
  // expiry, the tx should be cleaned up.
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Create a tx with low expiry.
  fib_client_t fib_client(account);
  SignedTransactionBatch txs;
  SignedTransaction tx{
      .tx = Transaction{.invocation =
                            fib_client.make_invocation<"slow_fibonacci">(
                                /*gas_amount=*/5'000'000'000, uint32_t{10}),
                        .uniquifier = {.block_expiry_height = 5, .nonce = 0},
                        .contracts_to_deploy = {},
                        .auth_claims = {},
                        .gas_claims = {}}};
  txs.push_back(tx);
  scheduler->add_txs(txs)();
  EXPECT_EQ(scheduler->size(), 1UL);

  // Advance the block past the expiry.
  for (int i = 0; i < 10; i++) {
    scheduler->increment_current_block();
  }
  EXPECT_GT(scheduler->current_block(), 5);

  // Schedule should remove the expired tx and return nothing.
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/10, scheduled_hashes, scheduled);
  EXPECT_EQ(scheduled.size(), 0UL);
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, BugHunt_ScheduleDiscardedTxAccountDoubleInsert)
{
  // Hypothesis: When an account has 3 txs where 1 is affordable, 1 is
  // unaffordable (gas > total hogs), and 1 more is affordable, the account
  // gets added to both removed_accounts and accounts_excluded_from_block
  // during schedule(). After the main loop,
  // cycle_accounts_during_schedule_helper may re-insert the account into
  // ready_accounts_ (because there's still a remaining tx), and then the
  // accounts_excluded_from_block loop tries to insert it again, potentially
  // causing a CHECK failure at line 668 of scheduler.cc.
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  // Give the account a moderate amount of hogs.
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/static_cast<uint128_t>(15'000'000'000));
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  fib_client_t fib_client(account);
  SignedTransactionBatch txs;

  // Create a normal affordable tx (gas = 5 billion).
  txs.push_back(SignedTransaction{
      .tx = Transaction{
          .invocation = fib_client.make_invocation<"slow_fibonacci">(
              /*gas_amount=*/5'000'000'000, uint32_t{10}),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 0},
          .contracts_to_deploy = {},
          .auth_claims = {},
          .gas_claims = {}}});

  // Create an unaffordable tx (gas = 100 billion >> 15 billion total).
  txs.push_back(SignedTransaction{
      .tx = Transaction{
          .invocation = fib_client.make_invocation<"slow_fibonacci">(
              /*gas_amount=*/100'000'000'000ULL, uint32_t{10}),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 1},
          .contracts_to_deploy = {},
          .auth_claims = {},
          .gas_claims = {}}});

  // Create another affordable tx (gas = 5 billion) -- this one stays in the
  // runqueue after the unaffordable tx is discarded.
  txs.push_back(SignedTransaction{
      .tx = Transaction{
          .invocation = fib_client.make_invocation<"slow_fibonacci">(
              /*gas_amount=*/5'000'000'000, uint32_t{10}),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 2},
          .contracts_to_deploy = {},
          .auth_claims = {},
          .gas_claims = {}}});

  scheduler->add_txs(txs)();
  EXPECT_EQ(scheduler->size(), 3UL);

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  // This may crash with a CHECK failure if the account is double-inserted
  // into ready_accounts_.
  scheduler->schedule(/*num_txs=*/100, scheduled_hashes, scheduled);

  // If we get here, the schedule completed without crashing.
  // At least the affordable txs should be handled (scheduled or available).
  EXPECT_GE(scheduled.size(), 1UL);
}

TEST(Scheduler, BugHunt_ScheduleWithCycleThenScheduleAgain)
{
  // Hypothesis: After scheduling with a cycle (hogs in already_scheduled get
  // cycled to to_schedule), scheduling again in a subsequent block should
  // correctly use the cycled hogs.
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  // Start with 0 hogs in to_schedule but 10B in already_scheduled.
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/10,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/5,
                  /*num_hogs=*/static_cast<uint128_t>(10'000'000'000));
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  // Add and schedule in block 1.
  scheduler->add_txs(txs)();
  EXPECT_EQ(scheduler->size(), 2UL);

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);

  // The cycle should have moved hogs from already_scheduled to to_schedule.
  EXPECT_EQ(scheduled.size(), 1UL);

  // Increment block and schedule again.
  scheduler->increment_current_block();
  scheduled_hashes.clear();
  scheduled.clear();
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
  EXPECT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, BugHunt_CycleGroupsLargeValues)
{
  // Hypothesis: cycle_groups with large uint128_t values near the total hog
  // supply should work without overflow.
  HogAge hog_age{};
  hog_age.address = MempoolTestInfra::kAccountAddresses[0];

  // Large values but well within uint128_t range.
  constexpr uint128_t kLargeHogs =
      static_cast<uint128_t>(1'000'000'000'000'000ULL);
  hog_age.num_hogs[kHogAgeGroupToSchedule] = to_xdr(kLargeHogs);
  hog_age.average_ages[kHogAgeGroupToSchedule] = 1'000'000;
  hog_age.num_hogs[kHogAgeGroupAlreadyScheduled] = to_xdr(kLargeHogs * 2);
  hog_age.average_ages[kHogAgeGroupAlreadyScheduled] = 500'000;
  hog_age.num_hogs[kHogAgeGroupReceived] = to_xdr(kLargeHogs * 3);
  hog_age.average_ages[kHogAgeGroupReceived] = 100'000;

  Scheduler::cycle_groups(hog_age);

  // to_schedule should have kLargeHogs + kLargeHogs * 2 = 3 * kLargeHogs
  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupToSchedule]), kLargeHogs * 3);
  // already_scheduled should have kLargeHogs * 3
  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            kLargeHogs * 3);
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupAlreadyScheduled], 100'000);
  // received should be 0
  EXPECT_EQ(from_xdr(hog_age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));

  // Verify average age of to_schedule:
  // (1T * 1M + 2T * 500K) / (1T + 2T) = (1e18 + 1e18) / 3e15 = 2e18/3e15
  // = 666.67 -> 667 (rounded up)
  // Actually: numerator = kLargeHogs * 1'000'000 + kLargeHogs * 2 * 500'000
  //                     = kLargeHogs * (1'000'000 + 1'000'000)
  //                     = kLargeHogs * 2'000'000
  // denominator = kLargeHogs * 3
  // result = 2'000'000 / 3 = 666666 + (2000000 % 3 != 0 ? 1 : 0)
  // 2000000 % 3 = 2, so +1 = 666667
  EXPECT_EQ(hog_age.average_ages[kHogAgeGroupToSchedule], 666'667);
}

TEST(Scheduler, BugHunt_ScheduleSameAccountAcrossBlocks)
{
  // Hypothesis: Hogs spent during scheduling are moved to already_scheduled,
  // then cycled back to to_schedule in subsequent blocks. So even with limited
  // hogs, all txs can eventually be scheduled across multiple blocks.
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/static_cast<uint128_t>(20'000'000'000));

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 10);
  scheduler->add_txs(txs)();

  size_t total_scheduled = 0;
  for (int block = 0; block < 5; block++) {
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduler->schedule(/*num_txs=*/2, scheduled_hashes, scheduled);
    total_scheduled += scheduled.size();
    scheduler->increment_current_block();
  }

  // Hogs are cycled back, so all 10 txs can be scheduled (2 per block * 5
  // blocks).
  EXPECT_EQ(total_scheduled, 10UL);
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, BugHunt_ReenqueueTxsNotInReadyAccounts)
{
  // Hypothesis 21: After reenqueue_txs, accounts are not re-added to
  // ready_accounts_, so reenqueued txs cannot be found by subsequent
  // schedule() calls. This is a potential bug where reenqueued txs
  // become stranded.
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);

  // Add txs (this puts account into ready_accounts_).
  scheduler->add_txs(txs)();
  EXPECT_EQ(scheduler->size(), 3UL);

  // Schedule all 3 txs (removes account from ready_accounts_ and
  // removes txs from runqueue).
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/3, scheduled_hashes, scheduled);
  ASSERT_EQ(scheduled.size(), 3UL);
  EXPECT_EQ(scheduler->size(), 0UL);

  // Reenqueue 2 of the 3 txs. This puts them back on the runqueue
  // and increments size_, but does NOT re-add the account to ready_accounts_.
  scheduler->reenqueue_txs(scheduled_hashes.begin(),
                           scheduled_hashes.begin() + 2);
  EXPECT_EQ(scheduler->size(), 2UL);

  // Now try to schedule the reenqueued txs. If ready_accounts_ is empty,
  // schedule() will not find any accounts to schedule from.
  scheduler->increment_current_block();
  x23::XdrVec<SignedTransactionHash> rescheduled_hashes;
  x23::XdrVec<SignedTransaction> rescheduled;
  scheduler->schedule(/*num_txs=*/10, rescheduled_hashes, rescheduled);

  // Does reenqueue_txs properly make txs available for rescheduling?
  EXPECT_EQ(rescheduled.size(), 2UL)
      << "Reenqueued txs should be schedulable. Got " << rescheduled.size()
      << " scheduled, " << scheduler->size() << " remaining in scheduler.";
}

TEST(Scheduler, BugHunt_ProcessTransfersNegativeDeltaSetsStaleAge)
{
  // Hypothesis 22: When a negative delta removes ALL hogs from a group,
  // the age of that group is left unchanged (stale), similar to the
  // confirmed cycle_groups bug.
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/42,
                  /*num_hogs=*/static_cast<uint128_t>(1'000));
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/99,
                  /*num_hogs=*/static_cast<uint128_t>(500));
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/7,
                  /*num_hogs=*/static_cast<uint128_t>(200));

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Withdraw all hogs: 200 from received, 500 from already_scheduled,
  // 1000 from to_schedule.
  HogsTransferred transfer;
  transfer.block = 20;
  transfer.deltas.push_back(
      Delta{.address = account,
            .hog_delta = to_xdr(static_cast<int128_t>(-1700))});
  scheduler->process_transfers(transfer)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  // All groups should be 0.
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));

  // BUG: The ages should be 0 (or at least well-defined) when the group
  // has 0 hogs. But process_transfers_helper doesn't reset ages when
  // zeroing out groups via negative deltas.
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 0)
      << "Stale age left for empty to_schedule group";
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 0)
      << "Stale age left for empty already_scheduled group";
  // Received group: num was deducted first (200 -> 0), age was not
  // touched. But age was 7, so it should be 0.
  EXPECT_EQ(age->average_ages[kHogAgeGroupReceived], 0)
      << "Stale age left for empty received group";
}

TEST(Scheduler, BugHunt_ScheduleWithInsufficientToScheduleForcesCycle)
{
  // Hypothesis 23: When to_schedule has fewer hogs than the gas limit
  // of the first tx, schedule() should trigger cycle_if_needed to pull
  // hogs from already_scheduled into to_schedule. Verify that the cycle
  // happens and the tx gets scheduled.
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  // to_schedule has 1B hogs (not enough for 5B gas tx).
  // already_scheduled has 10B hogs.
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/5,
                  /*num_hogs=*/static_cast<uint128_t>(1'000'000'000));
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/10,
                  /*num_hogs=*/static_cast<uint128_t>(10'000'000'000));
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  fib_client_t fib_client(account);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  scheduler->add_txs(txs)();
  EXPECT_EQ(scheduler->size(), 1UL);

  // Schedule: the tx needs 5B gas, to_schedule only has 1B.
  // cycle_if_needed should be triggered by schedule() when it sees
  // the account can't afford the tx but has enough total hogs.
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);

  // After cycling, to_schedule should have 1B + 10B = 11B, enough for 5B tx.
  EXPECT_EQ(scheduled.size(), 1UL)
      << "Should have cycled hogs from already_scheduled to schedule the tx";
  EXPECT_EQ(scheduler->size(), 0UL);
}

TEST(Scheduler, BugHunt_ScheduleAccountExcludedThenCycled)
{
  // Hypothesis 24: When an account has a tx whose gas exceeds total hogs,
  // the tx is discarded and the account is added to
  // accounts_excluded_from_block. Then at the end of schedule(),
  // cycle_accounts_during_schedule_helper re-inserts the account into
  // ready_accounts_ (if it has remaining txs), and the
  // accounts_excluded_from_block loop tries to CHECK-insert it again. The code
  // says "It is possible that the account is already in
  // accounts_excluded_from_block" but the CHECK at line 668 says
  // CHECK(ready_accounts_.insert(get_account_key(account)).second).
  //
  // We try to trigger this with:
  // 1. Two txs for the same account: one unaffordable, one affordable.
  // 2. The unaffordable tx is ordered BEFORE the affordable one in the map
  //    (by TxKey ordering, which is expiry then hash).
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                  /*num_hogs=*/static_cast<uint128_t>(10'000'000'000));
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0,
                  /*num_hogs=*/0);

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  fib_client_t fib_client(account);
  SignedTransactionBatch txs;

  // First tx: unaffordable (100B gas > 10B total hogs). Use max expiry so
  // it sorts first or similar in the TxKey map.
  txs.push_back(SignedTransaction{
      .tx = Transaction{
          .invocation = fib_client.make_invocation<"slow_fibonacci">(
              /*gas_amount=*/100'000'000'000ULL, uint32_t{10}),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 0},
          .contracts_to_deploy = {},
          .auth_claims = {},
          .gas_claims = {}}});

  // Second tx: affordable (5B gas <= 10B hogs). Same expiry so hash
  // determines ordering.
  txs.push_back(SignedTransaction{
      .tx = Transaction{
          .invocation = fib_client.make_invocation<"slow_fibonacci">(
              /*gas_amount=*/5'000'000'000, uint32_t{10}),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 1},
          .contracts_to_deploy = {},
          .auth_claims = {},
          .gas_claims = {}}});

  scheduler->add_txs(txs)();
  EXPECT_EQ(scheduler->size(), 2UL);

  // Schedule. The unaffordable tx should be discarded and the account
  // excluded. The affordable tx should remain (or be discarded too since
  // the account is excluded for the rest of the block).
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/100, scheduled_hashes, scheduled);

  // The unaffordable tx should be discarded (size decremented).
  // The affordable tx should either be scheduled or left for next block.
  // Key thing: no CHECK failure should occur.
  EXPECT_LE(scheduled.size(), 1UL);
}

TEST(Scheduler, BugHunt_NegativeDeltaExceedsReceived)
{
  // Hypothesis 25: What happens when a negative delta exceeds the received
  // group but there are hogs in already_scheduled and to_schedule to absorb it?
  // This should work correctly, deducting from received -> already_scheduled
  // -> to_schedule in order.
  const ContractAddress &account = MempoolTestInfra::kAccountAddresses[0];

  MempoolTestInfra server;
  server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/10,
                  /*num_hogs=*/static_cast<uint128_t>(5000));
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/5,
                  /*num_hogs=*/static_cast<uint128_t>(3000));
  server.set_hogs(account, kHogAgeGroupReceived, /*age=*/1,
                  /*num_hogs=*/static_cast<uint128_t>(1000));

  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Withdraw 6000 hogs total: 1000 from received, 3000 from already_scheduled,
  // 2000 from to_schedule.
  HogsTransferred transfer;
  transfer.block = 20;
  transfer.deltas.push_back(
      Delta{.address = account,
            .hog_delta = to_xdr(static_cast<int128_t>(-6000))});
  scheduler->process_transfers(transfer)();

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(3000));
  // Age for to_schedule should be unchanged at 10 (we just removed some
  // hogs but didn't change the age).
  EXPECT_EQ(age->average_ages[kHogAgeGroupToSchedule], 10);
}

} // namespace hogd
