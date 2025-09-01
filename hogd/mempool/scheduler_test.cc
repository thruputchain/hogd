#include <cstring>
#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/mempool_test_server.h"
#include "hogd/mempool/scheduler.h"
#include "types.hh"

namespace hogd {

namespace {

constexpr size_t kBlockSize = 100;

} // namespace

TEST(Scheduler, ReadAge)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(
      scheduler.read_average_hog_age({MempoolTestServer::kAccountAddresses[0]},
                                     /*force=*/true));
  HogAge &ret =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(ret.num_hogs[kHogAgeGroupToSchedule],
            // TODO(jhumphri): Update this, too.
            to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)));
  EXPECT_EQ(ret.average_ages[kHogAgeGroupToSchedule], 1);
}

TEST(Scheduler, WriteAge)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(
      scheduler.read_average_hog_age({MempoolTestServer::kAccountAddresses[0]},
                                     /*force=*/true));
  HogAge &ret =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(ret.num_hogs[kHogAgeGroupToSchedule],
            // TODO(jhumphri): Update this, too.
            to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)));
  EXPECT_EQ(ret.average_ages[kHogAgeGroupToSchedule], 1);

  // Now write a new hog balance.
  ret.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<uint128_t>(5));
  EXPECT_TRUE(scheduler.write_average_hog_age(
      {MempoolTestServer::kAccountAddresses[0]}));

  // Check that we read the new balance back.
  EXPECT_TRUE(
      scheduler.read_average_hog_age({MempoolTestServer::kAccountAddresses[0]},
                                     /*force=*/true));
  ret = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(ret.num_hogs[kHogAgeGroupToSchedule],
            to_xdr(static_cast<uint128_t>(5)));
  EXPECT_EQ(ret.average_ages[kHogAgeGroupToSchedule], 1);
}

TEST(Scheduler, AddVettedTx)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  scheduler.add_txs({txs[0]});
}

TEST(Scheduler, AddVettedTxDuplicate)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  scheduler.add_txs({txs[0]});
  // Now add the transaction a second time.
  scheduler.add_txs({txs[0]});
}

TEST(Scheduler, Single)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  scheduler.add_txs({txs[0]});
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  ASSERT_EQ(scheduled.size(), 1);
  EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
            MempoolTestServer::kAccountAddresses[0]);

  scheduler.mark_txs_executed({sha256(txs[0])}, /*block=*/1);
  // Check that the correct number of hogs was deducted.
  const HogAge &age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 9);

  // Check that the correct number of hogs was added to the
  // [kHogAgeGroupAlreadyScheduled] group.
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            5'000'000'000);
  // Due to rounding, the age of the [kHogAgeGroupAlreadyScheduled] will be
  // either equal to that of the [kHogAgeGroupToSchedule] or one less.
  EXPECT_LE(age.average_ages[kHogAgeGroupAlreadyScheduled],
            age.average_ages[kHogAgeGroupToSchedule]);
  EXPECT_GE(age.average_ages[kHogAgeGroupAlreadyScheduled],
            age.average_ages[kHogAgeGroupToSchedule] - 1);
}

TEST(Scheduler, TxFailed)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  {
    scheduler.add_txs({txs[0]});
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddresses[0]);
  }

  // Check that the correct number of hogs was deducted.
  HogAge age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 9);

  // Check that the correct number of hogs was added back.
  scheduler.mark_txs_failed({sha256(txs[0])});
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 10);

  // Check that the correct number of hogs was deducted again.
  {
    scheduler.add_txs({txs[0]});
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddresses[0]);
    age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
              5'000'000'000 * 9);
  }

  // Check that [mark_txs_executed()] does not change the number of hogs.
  scheduler.mark_txs_executed({sha256(txs[0])}, /*block=*/1);
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 9);
}

TEST(Scheduler, Empty)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  EXPECT_TRUE(scheduled.empty());
}

TEST(Scheduler, MultipleOneBlock)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kBlockSize);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule, /*age=*/1,
                  /*num_hogs=*/5'000'000'000 * kBlockSize);
  scheduler.add_txs(txs);
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  ASSERT_EQ(scheduled.size(), kBlockSize);
  for (const SignedTransaction &signed_tx : scheduled) {
    EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddresses[0]);
  }
}

TEST(Scheduler, MultipleMultipleBlocks)
{
  constexpr size_t kNumBlocks = 10;

  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs =
      generate_txs(fib_client, kBlockSize * kNumBlocks);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule, /*age=*/1,
                  /*num_hogs=*/5'000'000'000 * kBlockSize * kNumBlocks);
  scheduler.add_txs(txs);
  for (size_t i = 0; i < kNumBlocks; i++) {
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), kBlockSize);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddresses[0]);
    }
  }

  {
    // Now check that no transactions remain.
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }
}

TEST(Scheduler, MultipleMultipleBlocksNonMultiple)
{
  constexpr size_t kNumBlocks = 10;
  constexpr size_t kRemainder = 5;

  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs =
      generate_txs(fib_client, (kBlockSize * kNumBlocks) + kRemainder);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule, /*age=*/1,
                  /*num_hogs=*/5'000'000'000 *
                      (kBlockSize * kNumBlocks + kRemainder));
  scheduler.add_txs(txs);
  for (size_t i = 0; i < kNumBlocks; i++) {
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), kBlockSize);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddresses[0]);
    }
  }

  {
    // Now check the remainder.
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), kRemainder);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddresses[0]);
    }
  }

  {
    // Now check that no transactions remain.
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }
}

TEST(Scheduler, NotEnoughHogs)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(
      scheduler.read_average_hog_age({MempoolTestServer::kAccountAddresses[0]},
                                     /*force=*/false));
  HogAge &age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  age.average_ages[kHogAgeGroupToSchedule] = 1;
  age.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<uint128_t>(0));
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);

  scheduler.add_txs({txs[0]});
  // Calling [scheduler.update_accounts()] is unnecessary here, but we do it
  // anyway to test that it is a no-op.
  scheduler.update_accounts({MempoolTestServer::kAccountAddresses[0]});
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  EXPECT_TRUE(scheduled.empty());
}

TEST(Scheduler, NotEnoughHogsAfterAdded)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  scheduler.add_txs({txs[0]});
  // Setting the account's hogs to 0 *after* adding the tx to the scheduler
  // above.
  HogAge &age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  age.average_ages[kHogAgeGroupToSchedule] = 1;
  age.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<uint128_t>(0));
  scheduler.update_accounts({MempoolTestServer::kAccountAddresses[0]});
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  EXPECT_TRUE(scheduled.empty());
}

TEST(Scheduler, TwoAccounts)
{
  fib_client_t fib_client_a(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs_a = generate_txs(fib_client_a, kNumTxs);

  fib_client_t fib_client_b(MempoolTestServer::kAccountAddresses[1]);
  const SignedTransactionBatch txs_b = generate_txs(fib_client_b, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  scheduler.add_txs({txs_a[0], txs_b[0]});
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  ASSERT_EQ(scheduled.size(), 2);
  EXPECT_TRUE(txs_a[0] == scheduled[0] || txs_a[0] == scheduled[1]);
  EXPECT_TRUE(txs_b[0] == scheduled[0] || txs_b[0] == scheduled[1]);
}

TEST(Scheduler, MultipleAccounts)
{
  std::vector<fib_client_t> fib_clients;
  std::vector<SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestServer::kAccountAddresses) {
    fib_clients.emplace_back(address);
    txs.push_back(generate_txs(fib_clients.back(), kNumTxs));
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  for (const SignedTransactionBatch &tx : txs) {
    scheduler.add_txs({tx[0]});
  }
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  ASSERT_EQ(scheduled.size(), MempoolTestServer::kAccountAddresses.size());
}

TEST(Scheduler, UpdateGroupsBasic)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule, /*age=*/1, /*num_hogs=*/0);
  scheduler.add_txs({txs[0]});
  {
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }

  // Add enough hogs to the [kHogAgeGroupAlreadyScheduled] group.
  HogAge &age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  age.num_hogs[kHogAgeGroupAlreadyScheduled] =
      to_xdr(static_cast<uint128_t>(5'000'000'000 * 5));
  age.average_ages[kHogAgeGroupAlreadyScheduled] = 10;
  scheduler.cycle_groups(MempoolTestServer::kAccountAddresses[0]);

  {
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddresses[0]);
    EXPECT_EQ(age.num_hogs[kHogAgeGroupToSchedule],
              to_xdr(static_cast<uint128_t>(5'000'000'000 * 4)));
    EXPECT_EQ(age.num_hogs[kHogAgeGroupAlreadyScheduled],
              to_xdr(static_cast<uint128_t>(0)));
  }
}

TEST(Scheduler, ProcessReceivedHogs)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  scheduler.process_received_hogs(
      MempoolTestServer::kAccountAddresses[0],
      /*num_received=*/to_xdr(static_cast<uint128_t>(5'000'000'000)),
      /*current_block=*/10);
  HogAge age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 10);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 5'000'000'000 * 1);

  scheduler.process_received_hogs(
      MempoolTestServer::kAccountAddresses[0],
      /*num_received=*/to_xdr(static_cast<uint128_t>(5'000'000'000 * 4)),
      /*current_block=*/20);
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 18);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 5'000'000'000 * 5);
}

TEST(Scheduler, ProcessReceivedHogsAndUpdateGroups)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule, /*age=*/10,
                  /*num_hogs=*/5'000'000'000);
  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupAlreadyScheduled, /*age=*/25,
                  /*num_hogs=*/5'000'000'000 * 2);
  scheduler.process_received_hogs(
      MempoolTestServer::kAccountAddresses[0],
      /*num_received=*/to_xdr(static_cast<uint128_t>(5'000'000'000)),
      /*current_block=*/60);
  scheduler.process_received_hogs(
      MempoolTestServer::kAccountAddresses[0],
      /*num_received=*/to_xdr(static_cast<uint128_t>(5'000'000'000)),
      /*current_block=*/120);
  HogAge age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 90);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 5'000'000'000 * 2);

  scheduler.cycle_groups(MempoolTestServer::kAccountAddresses[0]);
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 20);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 3);
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 90);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            5'000'000'000 * 2);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 0);

  scheduler.cycle_groups(MempoolTestServer::kAccountAddresses[0]);
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 48);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 5);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]), 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 0);

  // Calling [scheduler.cycle_groups()] again should not do anything as there
  // are no hogs to move between groups (hogs in the [kHogAgeGroupToSchedule]
  // group cannot be moved to another group, and the other groups have no hogs).
  scheduler.cycle_groups(MempoolTestServer::kAccountAddresses[0]);
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 48);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 5);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]), 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 0);
}

TEST(Scheduler, Fairness)
{
  constexpr size_t kNumRounds = 100;

  std::vector<fib_client_t> fib_clients;
  std::vector<SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestServer::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs.push_back(generate_txs(fib_clients.back(), kBlockSize * kNumRounds));
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and hogs:
  //
  // Account 0: Age 3 and 5 billion hogs.
  // Account 1: Age 2 and 10 billion hogs.
  // Account 2: Age 1 and 15 billion hogs.
  // Account 3: Age 0 and 20 billion hogs.
  //
  // Thus, there are 50 billion hogs in total across the four accounts.
  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    server.set_hogs(MempoolTestServer::kAccountAddresses[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/MempoolTestServer::kAccountAddresses.size() - i - 1,
                    /*num_hogs=*/5'000'000'000 * (i + 1));
  }
  for (const SignedTransactionBatch &tx : txs) {
    scheduler.add_txs(tx);
  }

  std::unordered_map<ContractAddress, size_t> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    const auto &[scheduled_hashes, scheduled] = scheduler.schedule(kBlockSize);
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }
    scheduler.mark_txs_executed(scheduled_hashes, /*block=*/i);
    for (const ContractAddress &account :
         MempoolTestServer::kAccountAddresses) {
      scheduler.cycle_groups(account);
    }
  }

  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestServer::kAccountAddresses[i];
    EXPECT_EQ(tx_counts[address], (i + 1) * kNumRounds);
  }
}

TEST(Scheduler, FairnessAcrossBlocks)
{
  constexpr size_t kNumRounds = 100;

  std::vector<fib_client_t> fib_clients;
  std::vector<SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestServer::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs.push_back(generate_txs(fib_clients.back(), kBlockSize * kNumRounds));
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and hogs:
  //
  // Account 0: Age 3 and 5 billion hogs.
  // Account 1: Age 2 and 10 billion hogs.
  // Account 2: Age 1 and 15 billion hogs.
  // Account 3: Age 0 and 20 billion hogs.
  //
  // Thus, there are 50 billion hogs in total across the four accounts.
  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    server.set_hogs(MempoolTestServer::kAccountAddresses[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/MempoolTestServer::kAccountAddresses.size() - i - 1,
                    /*num_hogs=*/5'000'000'000 * (i + 1));
  }
  for (const SignedTransactionBatch &tx : txs) {
    scheduler.add_txs(tx);
  }

  std::unordered_map<ContractAddress, size_t> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    const auto &[scheduled_hashes, scheduled] =
        scheduler.schedule(/*block_size=*/1);
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }
    scheduler.mark_txs_executed(scheduled_hashes, /*block=*/i);
    for (const ContractAddress &account :
         MempoolTestServer::kAccountAddresses) {
      scheduler.cycle_groups(account);
    }
  }

  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestServer::kAccountAddresses[i];
    // TODO(jhumphri): Calculate this in a better way.
    size_t target = (i + 1) * 10;

    constexpr size_t kErrorBound = 4;
    EXPECT_LE(tx_counts[address], target + kErrorBound);
    EXPECT_GE(tx_counts[address], target - kErrorBound);
  }
}

TEST(Scheduler, YoungHogStarvation)
{
  constexpr size_t kNumRounds = 100;

  std::vector<fib_client_t> fib_clients;
  std::vector<SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestServer::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs.push_back(generate_txs(fib_clients.back(), kBlockSize * kNumRounds));
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and hogs:
  //
  // Account 0: Age 0 and (5 billion * kNumRounds) hogs.
  // Account 1: Age 1 and 5 billion hogs.
  // Account 2: Age 0 and 0 hogs.
  // Account 3: Age 0 and 0 hogs.
  //
  // Thus, there are 505 billion hogs in total across the four accounts.
  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/5'000'000'000 * kNumRounds);
  server.set_hogs(MempoolTestServer::kAccountAddresses[1],
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/1,
                  /*num_hogs=*/5'000'000'000);
  server.set_hogs(MempoolTestServer::kAccountAddresses[2],
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(MempoolTestServer::kAccountAddresses[3],
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/0);
  for (const SignedTransactionBatch &tx : txs) {
    scheduler.add_txs(tx);
  }

  std::unordered_map<ContractAddress, size_t> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    const auto &[scheduled_hashes, scheduled] =
        scheduler.schedule(/*block_size=*/1);
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }
    scheduler.mark_txs_executed(scheduled_hashes, /*block=*/i);
  }
  // All txs handled should have been submitted by account 0.
  EXPECT_EQ(tx_counts.size(), 1);
  EXPECT_EQ(tx_counts[MempoolTestServer::kAccountAddresses[0]], kNumRounds);

  {
    // The next tx scheduled should have been submitted by account 1.
    const auto &[_, scheduled] = scheduler.schedule(/*block_size=*/1);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddresses[1]);
  }
}

// TODO(jhumphri): Test that [cycle_groups()] is automatically called when
// needed. We should ensure that the new averages are being computed properly in
// [cycle_if_needed()].

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
