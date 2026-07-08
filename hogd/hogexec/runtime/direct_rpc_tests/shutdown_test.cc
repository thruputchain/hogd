#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <utility>

#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"

namespace hogd {

// Test against hogd/issues/493: UAF when hogdb closes a block before all
// pending transactions are closed.
// NOLINTNEXTLINE(misc-multiple-inheritance,misc-use-internal-linkage)
class ShutdownTest : public ::testing::Test,
                     public NativeContractTestFramework {
public:
  ShutdownTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  PerTransactionDatabaseProxy make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(
        std::numeric_limits<TransactionID>::max());
  }
};

TEST_F(ShutdownTest, AcceptedBlockUAF)
{
  auto addr = make_contract_address(0);
  setup_grant_storage_token(addr, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));

  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  EXPECT_THROW(advance_block(), std::runtime_error);

  auto res = tx1.abort_transaction();
  ASSERT_TRUE(res.has_value());
}

TEST_F(ShutdownTest, AcceptedBlockUAFUnclosedShutdown)
{
  auto addr = make_contract_address(0);
  setup_grant_storage_token(addr, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));

  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  EXPECT_THROW(advance_block(), std::runtime_error);
}

TEST_F(ShutdownTest, AbandonNoBlockClose)
{
  auto addr = make_contract_address(0);
  setup_grant_storage_token(addr, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));

  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.abort_transaction().has_value());
}

TEST_F(ShutdownTest, RejectedBlockUAF)
{
  auto addr = make_contract_address(0);
  setup_grant_storage_token(addr, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));

  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  discard_block();
}

} // namespace hogd
