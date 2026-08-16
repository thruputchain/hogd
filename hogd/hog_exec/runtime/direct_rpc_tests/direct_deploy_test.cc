#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
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

  static AddressAndKey metadata_key(ContractAddress const &addr)
  {
    return AddressAndKey{.address = addr,
                         .key = StorageKey(StorageKeyType::Metadata)};
  }

  // Queues a single deploy delta on [tx] and pushes it as a subtransaction,
  // returning whether the locks were acquired.
  static bool push_deploy(PerTransactionDatabaseProxy &tx,
                          AddressAndKey const &addrkey,
                          DeployedContractHash const &hash,
                          StorageDeltaFlagSet flags)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
    delta.deployed_contract_hash() = hash;
    tx.add_delta(addrkey, std::move(delta), flags);
    auto r = tx.try_push_current_subtransaction();
    CHECK(r.has_value());
    return r.value();
  }

  // Funds [addr], stores a deployable contract, and returns its hash.
  DeployedContractHash setup_deployable_address(ContractAddress const &addr)
  {
    setup_grant_storage_token(addr, 1'000'000'000);
    auto contract = make_native_contract(0);
    write_contract(contract);
    return make_deployed_contract_hash(contract);
  }
};

// An exclusive lock taken by a DeployContract delta blocks any concurrent
// reservation on the same address metadata key, on both backends. HogDb used
// to drop the flag on the floor (rpc_from_exec.cc did not forward flags to
// request_deploy_contract), so this diverged from MockDB: see issue #1189.
TEST_P(DirectDeployTest, ExclusiveLockConcurrentDeploy)
{
  auto contract_addr = make_contract_address(0);
  auto deployment_hash = setup_deployable_address(contract_addr);
  auto addrkey = metadata_key(contract_addr);

  // TX1: deploy with exclusive lock
  auto tx1 = make_new_per_tx_db_proxy();
  ASSERT_TRUE(
      push_deploy(tx1, addrkey, deployment_hash,
                  std::to_underlying(StorageDeltaFlags::ExclusiveLock)));

  // TX2: deploy same address, should be blocked by TX1's exclusive lock
  auto tx2 = make_new_per_tx_db_proxy();
  EXPECT_FALSE(push_deploy(tx2, addrkey, deployment_hash,
                           std::to_underlying(StorageDeltaFlags::None)))
      << "TX2 should be blocked by TX1's exclusive lock on the metadata key";

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  advance_block();

  auto deployed = lookup_deployment_from_db(contract_addr);
  ASSERT_TRUE(deployed.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(*deployed, deployment_hash);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

// Control for the test above: identical concurrent deploys are admitted
// side-by-side when neither asks for the exclusive lock, so the block above
// is attributable to the flag and not to deploys conflicting in general.
TEST_P(DirectDeployTest, NoExclusiveLockConcurrentDeployIsAdmitted)
{
  auto contract_addr = make_contract_address(1);
  auto deployment_hash = setup_deployable_address(contract_addr);
  auto addrkey = metadata_key(contract_addr);

  auto tx1 = make_new_per_tx_db_proxy();
  ASSERT_TRUE(push_deploy(tx1, addrkey, deployment_hash,
                          std::to_underlying(StorageDeltaFlags::None)));

  auto tx2 = make_new_per_tx_db_proxy();
  EXPECT_TRUE(push_deploy(tx2, addrkey, deployment_hash,
                          std::to_underlying(StorageDeltaFlags::None)));

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  advance_block();
}

// The contended direction: an exclusive deploy cannot be granted while
// another transaction already holds an ordinary reservation on the key.
TEST_P(DirectDeployTest, ExclusiveLockDeployBlockedByExistingHolder)
{
  auto contract_addr = make_contract_address(2);
  auto deployment_hash = setup_deployable_address(contract_addr);
  auto addrkey = metadata_key(contract_addr);

  auto tx1 = make_new_per_tx_db_proxy();
  ASSERT_TRUE(push_deploy(tx1, addrkey, deployment_hash,
                          std::to_underlying(StorageDeltaFlags::None)));

  auto tx2 = make_new_per_tx_db_proxy();
  EXPECT_FALSE(
      push_deploy(tx2, addrkey, deployment_hash,
                  std::to_underlying(StorageDeltaFlags::ExclusiveLock)))
      << "TX2 cannot take an exclusive lock while TX1 holds a reservation";

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  advance_block();
}

// The exclusive lock is permanent: committing the transaction that took it
// does not reopen the key for the rest of the block.
TEST_P(DirectDeployTest, ExclusiveLockDeployStaysLockedAfterCommit)
{
  auto contract_addr = make_contract_address(3);
  auto deployment_hash = setup_deployable_address(contract_addr);
  auto addrkey = metadata_key(contract_addr);

  auto tx1 = make_new_per_tx_db_proxy();
  ASSERT_TRUE(
      push_deploy(tx1, addrkey, deployment_hash,
                  std::to_underlying(StorageDeltaFlags::ExclusiveLock)));
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  auto tx2 = make_new_per_tx_db_proxy();
  EXPECT_FALSE(push_deploy(tx2, addrkey, deployment_hash,
                           std::to_underlying(StorageDeltaFlags::None)))
      << "the exclusive lock must outlive TX1's commit";
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  advance_block();

  auto deployed = lookup_deployment_from_db(contract_addr);
  ASSERT_TRUE(deployed.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(*deployed, deployment_hash);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

// An exclusive deploy locks its own address only.
TEST_P(DirectDeployTest, ExclusiveLockDeployDoesNotBlockOtherAddresses)
{
  auto locked_addr = make_contract_address(4);
  auto other_addr = make_contract_address(5);
  auto deployment_hash = setup_deployable_address(locked_addr);
  setup_grant_storage_token(other_addr, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  ASSERT_TRUE(
      push_deploy(tx1, metadata_key(locked_addr), deployment_hash,
                  std::to_underlying(StorageDeltaFlags::ExclusiveLock)));

  auto tx2 = make_new_per_tx_db_proxy();
  EXPECT_TRUE(push_deploy(tx2, metadata_key(other_addr), deployment_hash,
                          std::to_underlying(StorageDeltaFlags::None)));

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());
  advance_block();

  auto locked_deployment = lookup_deployment_from_db(locked_addr);
  ASSERT_TRUE(locked_deployment.has_value());
  auto other_deployment = lookup_deployment_from_db(other_addr);
  ASSERT_TRUE(other_deployment.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(*locked_deployment, deployment_hash);
  EXPECT_EQ(*other_deployment, deployment_hash);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectDeployTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

} // namespace hogd
