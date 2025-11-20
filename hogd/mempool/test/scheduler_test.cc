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

  x23::Vec<SignedTransactionHash> scheduled_hashes;
  x23::Vec<SignedTransaction> scheduled;
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

  x23::Vec<SignedTransactionHash> scheduled_hashes;
  x23::Vec<SignedTransaction> scheduled;
  scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);

  ASSERT_EQ(scheduled.size(), 1UL);
  EXPECT_EQ(scheduled.front().tx.invocation.invoked_address, account);

  const auto [age, lock] = scheduler->get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(age->average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(txs[0].tx.invocation.gas_limit));
}

TEST(Scheduler, FindAndRemoveFromRunqueue)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0], txs[1]})();
  EXPECT_EQ(scheduler->size(), 2UL);

  {
    x23::Vec<SignedTransactionHash> hashes;
    hashes.push_back(sha256(txs[0]));
    x23::Vec<SignedTransaction> removed;
    EXPECT_TRUE(scheduler->find_and_remove_from_runqueue(hashes, removed));
    ASSERT_EQ(removed.size(), 1UL);
    EXPECT_EQ(removed.front().tx.invocation.invoked_address,
              MempoolTestInfra::kAccountAddresses[0]);
    EXPECT_EQ(scheduler->size(), 1UL);
  }

  {
    SignedTransactionHash fake_hash{};
    fake_hash.fill(0xAA);
    x23::Vec<SignedTransactionHash> hashes;
    hashes.push_back(sha256(txs[1]));
    hashes.push_back(fake_hash);
    x23::Vec<SignedTransaction> removed;
    EXPECT_FALSE(scheduler->find_and_remove_from_runqueue(hashes, removed));
    EXPECT_TRUE(removed.empty());
    EXPECT_EQ(scheduler->size(), 1UL);
  }

  {
    x23::Vec<SignedTransactionHash> hashes;
    hashes.push_back(sha256(txs[1]));
    x23::Vec<SignedTransaction> removed;
    EXPECT_TRUE(scheduler->find_and_remove_from_runqueue(hashes, removed));
    ASSERT_EQ(removed.size(), 1UL);
    EXPECT_EQ(scheduler->size(), 0UL);
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
    x23::Vec<SignedTransactionHash> scheduled_hashes;
    x23::Vec<SignedTransaction> scheduled;
    scheduler->schedule(kBlockSize, scheduled_hashes, scheduled);
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }
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
    x23::Vec<SignedTransactionHash> scheduled_hashes;
    x23::Vec<SignedTransaction> scheduled;
    scheduler->schedule(/*num_txs=*/1, scheduled_hashes, scheduled);
    EXPECT_EQ(scheduled.size(), 1UL);
    tx_counts[scheduled.front().tx.invocation.invoked_address]++;
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

} // namespace hogd
