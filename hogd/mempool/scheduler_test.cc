#include <cstring>
#include <gtest/gtest.h>

#include <memory>

#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/mempool_test_server.h"
#include "hogd/mempool/scheduler.h"
#include "types.hh"

namespace hogd {

namespace {

constexpr size_t kBlockSize = 100;

} // namespace

TEST(Scheduler, ReadAndWriteAge)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(
      scheduler.read_average_hog_age({MempoolTestServer::kAccountAddress},
                                     /*force=*/true));
  HogAge ret;
  EXPECT_TRUE(
      scheduler.get_cached_hog_age(MempoolTestServer::kAccountAddress, ret));
  EXPECT_EQ(ret.num_hogs[kHogAgeGroupToSchedule],
            // TODO(jhumphri): Update this, too.
            to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)));
  EXPECT_EQ(ret.average_ages[kHogAgeGroupToSchedule], 1);
}

TEST(Scheduler, AddVettedTx)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(scheduler.add_vetted_txs({txs[0]}));
}

TEST(Scheduler, AddVettedTxDuplicate)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(scheduler.add_vetted_txs({txs[0]}));
  // Now add the transaction a second time.
  EXPECT_TRUE(scheduler.add_vetted_txs({txs[0]}));
}

TEST(Scheduler, ScheduleSingle)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(scheduler.add_vetted_txs({txs[0]}));
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  ASSERT_EQ(scheduled.size(), 1);
  EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
            MempoolTestServer::kAccountAddress);
}

TEST(Scheduler, ScheduleEmpty)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  EXPECT_TRUE(scheduled.empty());
}

TEST(Scheduler, ScheduleMultipleOneBlock)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kBlockSize);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(scheduler.add_vetted_txs(txs));
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  ASSERT_EQ(scheduled.size(), kBlockSize);
  for (const SignedTransaction &signed_tx : scheduled) {
    EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
              MempoolTestServer::kAccountAddress);
  }
}

TEST(Scheduler, ScheduleMultipleMultipleBlocks)
{
  constexpr size_t kNumBlocks = 10;

  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs =
      generate_txs(fib_client, kBlockSize * kNumBlocks);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(scheduler.add_vetted_txs(txs));
  for (size_t i = 0; i < kNumBlocks; i++) {
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), kBlockSize);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddress);
    }
  }

  {
    // Now check that no transactions remain.
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }
}

TEST(Scheduler, ScheduleMultipleMultipleBlocksNonMultiple)
{
  constexpr size_t kNumBlocks = 10;
  constexpr size_t kRemainder = 5;

  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs =
      generate_txs(fib_client, kBlockSize * kNumBlocks + kRemainder);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(scheduler.add_vetted_txs(txs));
  for (size_t i = 0; i < kNumBlocks; i++) {
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), kBlockSize);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddress);
    }
  }

  {
    // Now check the remainder.
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    ASSERT_EQ(scheduled.size(), kRemainder);
    for (const SignedTransaction &signed_tx : scheduled) {
      EXPECT_EQ(signed_tx.tx.invocation.invoked_address,
                MempoolTestServer::kAccountAddress);
    }
  }

  {
    // Now check that no transactions remain.
    const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
    EXPECT_TRUE(scheduled.empty());
  }
}

TEST(Scheduler, ScheduleNotEnoughHogs)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  server.set_hogs(MempoolTestServer::kAccountAddress, /*num_hogs=*/0);
  EXPECT_TRUE(scheduler.add_vetted_txs({txs[0]}));
  // Calling [scheduler.update_accounts()] is unnecessary here, but we do it
  // anyway to test that it is a no-op.
  CHECK(scheduler.update_accounts({MempoolTestServer::kAccountAddress}));
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  EXPECT_TRUE(scheduled.empty());
}

TEST(Scheduler, ScheduleNotEnoughHogsAfterAdded)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  EXPECT_TRUE(scheduler.add_vetted_txs({txs[0]}));
  // Setting the account's hogs to 0 *after* adding the tx to the scheduler
  // above.
  server.set_hogs(MempoolTestServer::kAccountAddress, /*num_hogs=*/0);
  CHECK(scheduler.update_accounts({MempoolTestServer::kAccountAddress}));
  const auto &[_, scheduled] = scheduler.schedule(kBlockSize);
  EXPECT_TRUE(scheduled.empty());
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
