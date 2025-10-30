#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "db_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr size_t kBlockSize = 100;
constexpr size_t kNumTxs = 100;

} // namespace

TEST(NewScheduler, ReadAge)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<NewScheduler> scheduler =
      server.mempool_coord_rpc().scheduler();

  scheduler->read_average_hog_ages({MempoolTestInfra::kAccountAddresses[0]})();
  HogAge &ret =
      scheduler->get_cached_hog_age(MempoolTestInfra::kAccountAddresses[0]);
  EXPECT_EQ(ret.num_hogs[kHogAgeGroupToSchedule],
            // TODO(jhumphri): Update this, too.
            to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)));
  EXPECT_EQ(ret.average_ages[kHogAgeGroupToSchedule], 0);
}

TEST(NewScheduler, AddVettedTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<NewScheduler> scheduler =
      server.mempool_coord_rpc().scheduler();

  EXPECT_EQ(scheduler->size(), 0UL);
  scheduler->add_txs({txs[0]})();
  EXPECT_EQ(scheduler->size(), 1UL);
}

TEST(NewScheduler, AddVettedTxDuplicate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<NewScheduler> scheduler =
      server.mempool_coord_rpc().scheduler();

  EXPECT_EQ(scheduler->size(), 0UL);
  scheduler->add_txs({txs[0]})();
  EXPECT_EQ(scheduler->size(), 1UL);
  // Now add the transaction a second time.
  scheduler->add_txs({txs[0]})();
  EXPECT_EQ(scheduler->size(), 1UL);
}

TEST(NewScheduler, Single)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  std::shared_ptr<NewScheduler> scheduler =
      server.mempool_coord_rpc().scheduler();

  scheduler->add_txs({txs[0]})();
  // Check that the correct number of hogs was deducted.
  const HogAge &age =
      scheduler->get_cached_hog_age(MempoolTestInfra::kAccountAddresses[0]);
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 10);
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 0);

  const auto &[_, scheduled] = scheduler->schedule(kBlockSize);
  ASSERT_EQ(scheduled.size(), 1);
  EXPECT_EQ(scheduled[0].tx.invocation.invoked_address,
            MempoolTestInfra::kAccountAddresses[0]);

  // Check the correct number of hogs was deducted from the
  // [kHogAgeGroupToSchedule] group.
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupToSchedule]), 5'000'000'000 * 9);
  EXPECT_EQ(age.average_ages[kHogAgeGroupToSchedule], 0);

  // Check that the correct number of hogs was added to the
  // [kHogAgeGroupAlreadyScheduled] group.
  EXPECT_EQ(from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]),
            5'000'000'000);
  EXPECT_EQ(age.average_ages[kHogAgeGroupAlreadyScheduled],
            age.average_ages[kHogAgeGroupToSchedule]);
}

} // namespace hogd
