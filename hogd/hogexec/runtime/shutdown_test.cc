#include <gtest/gtest.h>

#include <limits>

#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"

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
    auto tx1 = make_new_per_tx_db_proxy();

    tx1.add_delta(make_bytestring_delta("test"), std::to_underlying(StorageItemRPCFlags::kStorageNone));
}

}
