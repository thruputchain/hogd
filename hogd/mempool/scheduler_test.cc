#include <cstring>
#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "db_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/mempool_test_server.h"
#include "hogd/mempool/scheduler.h"
#include "transaction.hh"
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
  const auto &[_, scheduled] =
      scheduler.schedule(/*block_height=*/0, kBlockSize);
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
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
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
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/1, kBlockSize);
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

TEST(Scheduler, TxFailedStrandedEnqueued)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  server.set_hogs(MempoolTestServer::kAccountAddresses[0],
                  /*group=*/kHogAgeGroupToSchedule, /*age=*/1,
                  /*num_hogs=*/5'000'000'000);
  {
    scheduler.add_txs({txs[0], txs[1]});
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddresses[0]);
  }

  // Check that the correct number of hogs was deducted.
  HogAge age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);

  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/1, kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }

  scheduler.mark_txs_failed({sha256(txs[0])});
  // Check that txs[1] is now enqueued.
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
  {
    const auto &[hashes, _] =
        scheduler.schedule(/*block_height=*/2, kBlockSize);
    ASSERT_EQ(hashes.size(), 1);
    EXPECT_EQ(hashes[0], sha256(txs[1]));
  }

  // Check that [mark_txs_executed()] does not change the number of hogs.
  scheduler.mark_txs_executed({sha256(txs[1])}, /*block=*/1);
  age = scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/3, kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }
}

TEST(Scheduler, Empty)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const auto &[_, scheduled] =
      scheduler.schedule(/*block_height=*/0, kBlockSize);
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
  const auto &[_, scheduled] =
      scheduler.schedule(/*block_height=*/0, kBlockSize);
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
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
    ASSERT_EQ(scheduled.size(), kBlockSize);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddresses[0]);
    }
  }

  {
    // Now check that no transactions remain.
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/1, kBlockSize);
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
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
    ASSERT_EQ(scheduled.size(), kBlockSize);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddresses[0]);
    }
  }

  {
    // Now check the remainder.
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/1, kBlockSize);
    ASSERT_EQ(scheduled.size(), kRemainder);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddresses[0]);
    }
  }

  {
    // Now check that no transactions remain.
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/2, kBlockSize);
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
  const auto &[_, scheduled] =
      scheduler.schedule(/*block_height=*/0, kBlockSize);
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
  const auto &[_, scheduled] =
      scheduler.schedule(/*block_height=*/0, kBlockSize);
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
  const auto &[_, scheduled] =
      scheduler.schedule(/*block_height=*/0, kBlockSize);
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
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
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
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/1, kBlockSize);
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

TEST(Scheduler, ProcessReceivedHogsSchedule)
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

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]), 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 0);

  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/1, kBlockSize);
    EXPECT_TRUE(scheduled.empty());
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]), 0);
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 0);
  }

  scheduler.process_received_hogs(
      MempoolTestServer::kAccountAddresses[0],
      /*num_received=*/to_xdr(static_cast<uint128_t>(5'000'000'000)),
      /*current_block=*/2);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 2);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]), 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 0);
  {
    const auto &[scheduled_hashes, scheduled] =
        scheduler.schedule(/*block_height=*/3, kBlockSize);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0], txs[0]);
    scheduler.mark_txs_executed(scheduled_hashes, /*block=*/3);

    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
              5'000'000'000);
    EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 3);
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 0);
  }
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
    const auto &[scheduled_hashes, scheduled] =
        scheduler.schedule(/*block_height=*/i, kBlockSize);
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }
    scheduler.mark_txs_executed(scheduled_hashes, /*block=*/i);
  }

  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestServer::kAccountAddresses[i];
    EXPECT_EQ(tx_counts[address], (i + 1) * kNumRounds);
  }
}

TEST(Scheduler, FairnessAcrossBlocks)
{
  constexpr size_t kNumRounds = 100;
  static_assert(kNumRounds % 10 == 0);

  std::vector<fib_client_t> fib_clients;
  std::unordered_map<ContractAddress, SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestServer::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs[address] = generate_txs(fib_clients.back(), kBlockSize * kNumRounds);
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and hogs:
  //
  // Account 0: Age 0 and 5 billion hogs.  (Will get 10% of the throughput.)
  // Account 1: Age 0 and 10 billion hogs. (Will get 20% of the throughput.)
  // Account 2: Age 0 and 15 billion hogs. (Will get 30% of the throughput.)
  // Account 3: Age 0 and 20 billion hogs. (Will get 40% of the throughput.)
  //
  // Thus, there are 50 billion hogs in total across the four accounts.
  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    server.set_hogs(MempoolTestServer::kAccountAddresses[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/0,
                    /*num_hogs=*/5'000'000'000 * (i + 1));
  }
  for (const auto &[_, tx] : txs) {
    scheduler.add_txs(tx);
  }

  std::unordered_map<ContractAddress, size_t> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    const auto &[scheduled_hashes, scheduled] =
        scheduler.schedule(/*block_height=*/i, /*block_size=*/1);
    EXPECT_EQ(scheduled.size(), 1UL);
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }
    scheduler.mark_txs_executed(scheduled_hashes, /*block=*/i);
  }

  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestServer::kAccountAddresses[i];
    const size_t target = (i + 1) * (kNumRounds / 10);
    constexpr size_t kErrorBound = 2;
    EXPECT_LE(tx_counts[address], target + kErrorBound);
    EXPECT_GE(tx_counts[address], target - kErrorBound);
  }
}

TEST(Scheduler, FairnessAcrossBlocksLargeBlocks)
{
  constexpr size_t kNumRounds = 100;
  static_assert(kNumRounds % 10 == 0);

  std::vector<fib_client_t> fib_clients;
  std::unordered_map<ContractAddress, SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestServer::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs[address] = generate_txs(fib_clients.back(), kBlockSize * kNumRounds);
  }

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  // Sets the age and hogs for all accounts. Each account has this age and hogs:
  //
  // Account 0: Age 0 and 5 billion hogs.  (Will get 10% of the throughput.)
  // Account 1: Age 0 and 10 billion hogs. (Will get 20% of the throughput.)
  // Account 2: Age 0 and 15 billion hogs. (Will get 30% of the throughput.)
  // Account 3: Age 0 and 20 billion hogs. (Will get 40% of the throughput.)
  //
  // Thus, there are 50 billion hogs in total across the four accounts.
  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    server.set_hogs(MempoolTestServer::kAccountAddresses[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/0,
                    /*num_hogs=*/5'000'000'000 * (i + 1) * kBlockSize);
  }
  for (const auto &[_, tx] : txs) {
    scheduler.add_txs(tx);
  }

  std::unordered_map<ContractAddress, size_t> tx_counts;
  for (size_t i = 0; i < kNumRounds; i++) {
    const auto &[scheduled_hashes, scheduled] =
        scheduler.schedule(/*block_height=*/i, kBlockSize);
    EXPECT_LE(scheduled.size(), kBlockSize);
    for (const SignedTransaction &tx : scheduled) {
      tx_counts[tx.tx.invocation.invoked_address]++;
    }
    scheduler.mark_txs_executed(scheduled_hashes, /*block=*/i);
  }

  for (size_t i = 0; i < MempoolTestServer::kAccountAddresses.size(); i++) {
    const ContractAddress &address = MempoolTestServer::kAccountAddresses[i];
    const size_t target = (i + 1) * (kNumRounds * kBlockSize / 10);
    constexpr size_t kErrorBound = 2;
    EXPECT_LE(tx_counts[address], target + kErrorBound);
    EXPECT_GE(tx_counts[address], target - kErrorBound);
  }
}

TEST(Scheduler, YoungHogStarvation)
{
  constexpr size_t kNumRounds = 10;

  std::vector<fib_client_t> fib_clients;
  std::vector<SignedTransactionBatch> txs;
  for (const ContractAddress &address : MempoolTestServer::kAccountAddresses) {
    fib_clients.emplace_back(address);
    // We create enough txs for each account such that each account could fill
    // all blocks with txs. In other words, an account always have txs to
    // schedule. This is important for ensuring that when we see a certain
    // throughput of txs for a given account, this throughput resulted solely
    // from the scheduling policy rather than a lack of txs to schedule.
    txs.push_back(generate_txs(fib_clients.back(), kNumRounds * 2));
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
        scheduler.schedule(/*block_height=*/i, /*block_size=*/1);
    EXPECT_EQ(scheduled.size(), 1);
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
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/kNumRounds + 1, /*block_size=*/1);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddresses[1]);
  }
}

TEST(Scheduler, CycleFromAlreadyScheduled)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/0,
                  /*num_hogs=*/10'000'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/0,
                  /*num_hogs=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(10'000'000'000));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(10'000'000'000 -
                                   txs[0].tx.invocation.gas_limit));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, CycleFromReceived)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/0,
                  /*num_hogs=*/0);
  scheduler.process_received_hogs(
      account, /*num_received=*/to_xdr(static_cast<uint128_t>(15'000'000'000)),
      /*current_block=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(15'000'000'000));

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(15'000'000'000 -
                                   txs[0].tx.invocation.gas_limit));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, CycleFromAlreadyScheduledNotEnough)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/0,
                  /*num_hogs=*/4'000'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/0,
                  /*num_hogs=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(4'000'000'000));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(4'000'000'000));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, CycleFromReceivedNotEnough)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/0,
                  /*num_hogs=*/0);
  scheduler.process_received_hogs(
      account, /*num_received=*/to_xdr(static_cast<uint128_t>(4'000'000'000)),
      /*current_block=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(4'000'000'000));

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(4'000'000'000));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, CycleTotalIsEnough)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/0,
                  /*num_hogs=*/3'000'000'000);
  scheduler.process_received_hogs(
      account, /*num_received=*/to_xdr(static_cast<uint128_t>(4'000'000'000)),
      /*current_block=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(3'000'000'000));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(4'000'000'000));

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(7'000'000'000 -
                                   txs[0].tx.invocation.gas_limit));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, CycleTotalIsNotEnough)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/0,
                  /*num_hogs=*/0);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/0,
                  /*num_hogs=*/3'000'000'000);
  scheduler.process_received_hogs(
      account, /*num_received=*/to_xdr(static_cast<uint128_t>(1'000'000'000)),
      /*current_block=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(3'000'000'000));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(1'000'000'000));

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(4'000'000'000));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
}

TEST(Scheduler, CycleOlderAgeInAlreadyScheduled)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/1,
                  /*num_hogs=*/0);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/0,
                  /*num_hogs=*/4'000'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/1,
                  /*num_hogs=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 1);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(4'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 1);

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(4'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, CycleOlderAgeInReceived)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/1,
                  /*num_hogs=*/1);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/1,
                  /*num_hogs=*/0);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/0,
                  /*num_hogs=*/4'000'000'000);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(1));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 1);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 1);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(4'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(4'000'000'001));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 1);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, CycleCombineAges)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/80,
                  /*num_hogs=*/1'000'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/5,
                  /*num_hogs=*/2'000'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/0,
                  /*num_hogs=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(1'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 80);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(2'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 5);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(3'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 30);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, CycleCombineAgesAllGroups)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/37,
                  /*num_hogs=*/1'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/10,
                  /*num_hogs=*/2'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/7,
                  /*num_hogs=*/1'000'000);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(1'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 37);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(2'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 10);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(1'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 7);

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(4'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 16);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, CycleCombineAgesRoundUp)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/10,
                  /*num_hogs=*/2'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/5,
                  /*num_hogs=*/1'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/0,
                  /*num_hogs=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(2'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 10);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(1'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 5);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(3'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 9);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, CycleCombineAgesRoundUpAllGroups)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/14,
                  /*num_hogs=*/1'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/10,
                  /*num_hogs=*/2'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/5,
                  /*num_hogs=*/1'000'000);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(1'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 14);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(2'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 10);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(1'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 5);

  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(4'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 11);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, ProcessReceivedHogsRoundUp)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/0,
                  /*num_hogs=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);

  scheduler.process_received_hogs(
      account,
      /*num_received=*/to_xdr(static_cast<uint128_t>(1'000'000)),
      /*current_block=*/5);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(1'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 5);

  scheduler.process_received_hogs(
      account,
      /*num_received=*/to_xdr(static_cast<uint128_t>(2'000'000)),
      /*current_block=*/10);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(3'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 9);
}

TEST(Scheduler, AlreadyScheduledRoundUp)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const ContractAddress &account = MempoolTestServer::kAccountAddresses[0];
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupToSchedule,
                  /*age=*/1,
                  /*num_hogs=*/5'000'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupAlreadyScheduled,
                  /*age=*/3,
                  /*num_hogs=*/10'000'000'000);
  server.set_hogs(account,
                  /*group=*/kHogAgeGroupReceived,
                  /*age=*/0,
                  /*num_hogs=*/0);

  ASSERT_TRUE(scheduler.read_average_hog_age({account}, /*force=*/false));
  const HogAge &age = scheduler.get_cached_hog_age(account);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(5'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 1);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(10'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 3);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);

  {
    scheduler.add_txs({txs[0]});
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddresses[0]);
    scheduler.mark_txs_executed({sha256(txs[0])}, /*block=*/11);
  }

  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(0));
  // The age for the [kHogAgeGroupToSchedule] remains 1 until [cycle_groups()]
  // is called again.
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 1);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(15'000'000'000));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 6);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);

  scheduler.add_txs({txs[1]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]),
            static_cast<uint128_t>(15'000'000'000 -
                                   txs[1].tx.invocation.gas_limit));
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 6);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled], 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(0));
  EXPECT_EQ(age.average_ages[kHogAgeGroupReceived], 0);
}

TEST(Scheduler, Expired)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  txs[0].tx.uniquifier.block_expiry_height = 5;
  scheduler.add_txs({txs[0]});

  // Check that the correct number of hogs was deducted.
  const HogAge &age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 9);

  const auto &[_, scheduled] =
      scheduler.schedule(/*block_height=*/6, kBlockSize);
  EXPECT_TRUE(scheduled.empty());

  // Check that the correct number of hogs was added back to the
  // [kHogAgeGroupToSchedule] group.
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 10);
}

TEST(Scheduler, ExpiredStranded)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

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

  txs[0].tx.uniquifier.block_expiry_height = 5;
  scheduler.add_txs({txs[0]});
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);

  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/6, kBlockSize);
    EXPECT_TRUE(scheduled.empty());
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
  }

  // The tx should have been discarded because it has expired. We check that
  // adding enough hogs to run the tx does not cause it to be scheduled.
  scheduler.process_received_hogs(
      MempoolTestServer::kAccountAddresses[0],
      /*num_received=*/to_xdr(static_cast<uint128_t>(5'000'000'000)),
      /*current_block=*/7);
  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/8, kBlockSize);
    EXPECT_TRUE(scheduled.empty());
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
    EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 5'000'000'000);
  }
}

TEST(Scheduler, Filter)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  scheduler.add_txs({txs[0]});
  scheduler.filter_txs_for_account(MempoolTestServer::kAccountAddresses[0],
                                   [&txs](const SignedTransaction &tx) {
                                     EXPECT_EQ(tx, txs[0]);
                                     return false;
                                   });
  // Check that the hogs were refunded to the account.
  const HogAge &age =
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 10);
  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }

  // Now do the test again but do not filter the tx out.
  scheduler.add_txs({txs[0]});
  scheduler.filter_txs_for_account(MempoolTestServer::kAccountAddresses[0],
                                   [&txs](const SignedTransaction &tx) {
                                     EXPECT_EQ(tx, txs[0]);
                                     return true;
                                   });
  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0], txs[0]);
  }
  scheduler.mark_txs_executed({sha256(txs[0])}, /*block=*/1);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 9);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            5'000'000'000);
}

TEST(Scheduler, FilterStranded)
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

  scheduler.add_txs({txs[0]});
  scheduler.filter_txs_for_account(MempoolTestServer::kAccountAddresses[0],
                                   [&txs](const SignedTransaction &tx) {
                                     EXPECT_EQ(tx, txs[0]);
                                     return false;
                                   });
  scheduler.process_received_hogs(
      MempoolTestServer::kAccountAddresses[0],
      /*num_received=*/to_xdr(static_cast<uint128_t>(5'000'000'000)),
      /*current_block=*/1);
  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }

  // Now do the test again but do not filter the tx out.
  EXPECT_EQ(age.num_hogs[kHogAgeGroupToSchedule],
            to_xdr(static_cast<uint128_t>(0)));
  EXPECT_EQ(age.num_hogs[kHogAgeGroupAlreadyScheduled],
            to_xdr(static_cast<uint128_t>(0)));
  EXPECT_EQ(age.num_hogs[kHogAgeGroupReceived],
            to_xdr(static_cast<uint128_t>(5'000'000'000)));
  age.average_ages[kHogAgeGroupReceived] = 1;
  age.num_hogs[kHogAgeGroupReceived] = to_xdr(static_cast<uint128_t>(0));

  scheduler.add_txs({txs[0]});
  scheduler.filter_txs_for_account(MempoolTestServer::kAccountAddresses[0],
                                   [&txs](const SignedTransaction &tx) {
                                     EXPECT_EQ(tx, txs[0]);
                                     return true;
                                   });
  scheduler.process_received_hogs(
      MempoolTestServer::kAccountAddresses[0],
      /*num_received=*/to_xdr(static_cast<uint128_t>(5'000'000'000)),
      /*current_block=*/1);
  {
    const auto &[_, scheduled] =
        scheduler.schedule(/*block_height=*/0, kBlockSize);
    ASSERT_EQ(scheduled.size(), 1);
    EXPECT_EQ(scheduled[0], txs[0]);
  }
  scheduler.mark_txs_executed({sha256(txs[0])}, /*block=*/1);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 0);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            5'000'000'000);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupReceived]), 0);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
