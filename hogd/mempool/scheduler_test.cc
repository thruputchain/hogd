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

namespace {

constexpr ContractAddress kAccountAddress =
    NativeContractTestFramework::make_contract_address(10);

} // namespace

TEST(Scheduler, ReadAndWriteAge)
{
  fib_client_t fib_client(kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler scheduler = clients.interface->scheduler();

  HogAge average;
  memset(&average, 0, sizeof(average));
  average.address = kAccountAddress;
  average.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<int128_t>(10));
  average.average_ages[kHogAgeGroupToSchedule] = 1;

  EXPECT_TRUE(scheduler.write_average_hog_age({average}));
  HogAgeBatch ret;
  EXPECT_TRUE(scheduler.read_average_hog_age({average.address}, ret));
  ASSERT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].num_hogs[kHogAgeGroupToSchedule],
            to_xdr(static_cast<int128_t>(10)));
  EXPECT_EQ(ret[0].average_ages[kHogAgeGroupToSchedule], 1);
}

TEST(Scheduler, AddVettedTx)
{
  fib_client_t fib_client(kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler scheduler = clients.interface->scheduler();

  HogAge average;
  memset(&average, 0, sizeof(average));
  average.address = kAccountAddress;
  average.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<int128_t>(10));
  average.average_ages[kHogAgeGroupToSchedule] = 1;

  EXPECT_TRUE(scheduler.write_average_hog_age({average}));
  EXPECT_TRUE(scheduler.add_vetted_tx({txs[0]}));
}

TEST(Scheduler, AddVettedTxDuplicate)
{
  fib_client_t fib_client(kAccountAddress);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestServer server;
  Clients clients = create_clients(server);
  Scheduler scheduler = clients.interface->scheduler();

  HogAge average;
  memset(&average, 0, sizeof(average));
  average.address = kAccountAddress;
  average.num_hogs[kHogAgeGroupToSchedule] = to_xdr(static_cast<int128_t>(10));
  average.average_ages[kHogAgeGroupToSchedule] = 1;

  EXPECT_TRUE(scheduler.write_average_hog_age({average}));
  EXPECT_TRUE(scheduler.add_vetted_tx({txs[0]}));
  // Now add the transaction a second time.
  EXPECT_TRUE(scheduler.add_vetted_tx({txs[0]}));
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
