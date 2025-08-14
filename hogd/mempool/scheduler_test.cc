#include <cstring>
#include <gtest/gtest.h>

#include <memory>

#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/mempool/mempool.h"
#include "hogd/mempool/mempool_test_server.h"
#include "hogd/mempool/scheduler.h"
#include "types.hh"

namespace hogd {

TEST(Scheduler, ReadAndWriteAge)
{
  fib_client_t fib_client(MempoolTestServer::kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler &scheduler = clients.interface->scheduler();

  HogAgeBatch ret;
  EXPECT_TRUE(
      scheduler.read_average_hog_age({MempoolTestServer::kAccountAddress},
                                     ret));
  ASSERT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].num_hogs[kHogAgeGroupToSchedule],
            to_xdr(static_cast<int128_t>(10)));
  EXPECT_EQ(ret[0].average_ages[kHogAgeGroupToSchedule], 1);
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
  const auto &[_, scheduled] = scheduler.schedule(/*block_size=*/100);
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

  const auto &[_, scheduled] = scheduler.schedule(/*block_size=*/100);
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
