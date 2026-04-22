#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

class DirectDeployTest : public ::testing::TestWithParam<TestDB>,
                         public NativeContractTestFramework {
public:
  DirectDeployTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  auto make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }
};

// HogDB silently drops the exclusive lock flag on DeployContract
// (rpc_from_exec.cc doesn't pass flags to request_deploy_contract).
// MockDB correctly processes it. This means concurrent deploys to the same
// address with exclusive lock will diverge: MockDB blocks TX2, HogDB doesn't.
TEST_P(DirectDeployTest, ExclusiveLockConcurrentDeploy)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP()
        << "HogDB silently drops exclusive lock flag on DeployContract "
           "(rpc_from_exec.cc doesn't pass flags to request_deploy_contract). "
           "See https://github.com/groundhogchain/hogd/issues/1189";
  }

  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto contract = make_native_contract(0);
  write_contract(contract);
  auto deployment_hash = make_deployed_contract_hash(contract);

  AddressAndKey addrkey{.address = contract_addr,
                        .key = StorageKey(StorageKeyType::Metadata)};

  // TX1: deploy with exclusive lock
  auto tx1 = make_new_per_tx_db_proxy();
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
    delta.deployed_contract_hash() = deployment_hash;
    tx1.add_delta(addrkey, std::move(delta),
                  std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  }
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  // TX2: deploy same address, should be blocked by TX1's exclusive lock
  auto tx2 = make_new_per_tx_db_proxy();
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
    delta.deployed_contract_hash() = deployment_hash;
    tx2.add_delta(addrkey, std::move(delta),
                  std::to_underlying(StorageDeltaFlags::None));
  }
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  EXPECT_FALSE(r2.value())
      << "TX2 should be blocked by TX1's exclusive lock on the metadata key";

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  advance_block();
}

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectDeployTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
