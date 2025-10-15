#include <gtest/gtest.h>

#include <limits>
#include <utility>

#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"

namespace hogd {

// Test against hogd/issues/493: UAF when hogdb closes a block before all pending transactions are closed.
class ShutdownTest : public ::testing::Test, public NativeContractTestFramework {
public:
  ShutdownTest() : NativeContractTestFramework(false, BlockExecutionMode::kBlockExecutionModeProposal, TestDB::kHogDB) {}

  PerTransactionDatabaseProxy make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(std::numeric_limits<TransactionID>::max());
  }
};

TEST_F(ShutdownTest, Shutdown)
{
    auto addr = make_contract_address(0);
    setup_grant_storage_token(addr, 1'000'000'000);

    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(make_contract_data_addrkey(addr, "test"), make_bytestring_delta("value"),  std::to_underlying(StorageItemRPCFlags::kStorageNone));

    auto r1 = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1.value());

    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(make_contract_data_addrkey(addr, "test"), make_bytestring_delta("value"),  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    tx2.add_delta(make_contract_data_addrkey(addr, "test1"), make_bytestring_delta("value"),  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    
    auto r2 = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2.value());

    auto tx3 = make_new_per_tx_db_proxy();
    tx3.add_delta(make_contract_data_addrkey(addr, "test2"), make_bytestring_delta("value"),  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r3 = tx3.try_push_current_subtransaction();
    ASSERT_TRUE(r3.has_value());
    ASSERT_TRUE(r3.value());

    tx3.add_delta(make_contract_data_addrkey(addr, "test2"), make_bytestring_delta("value_longer"),  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r3_1 = tx3.try_push_current_subtransaction();
    ASSERT_TRUE(r3_1.has_value());
    ASSERT_TRUE(r3_1.value());

    //discard_block();

    // tx3 now has an exclusive lock

  //  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

}

}
