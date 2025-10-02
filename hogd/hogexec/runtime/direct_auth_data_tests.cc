#include <gtest/gtest.h>

#include <utility>

#include "account.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

class DirectAuthDataTest : public ::testing::TestWithParam<TestDB>,
                           public NativeContractTestFramework {
public:
  DirectAuthDataTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  void SetUp() override { cost_model_ = get_default_cost_model(); }

  static auto make_pk_config(Weight w)
  {
    auto [pk, _] = ed25519_keygen();
    PublicKey pk_wrapper(CryptographicScheme::Ed25519);
    pk_wrapper.ed25519() = pk;
    return x23::Ptr<PublicKeyConfig>(
        new PublicKeyConfig{.key = pk_wrapper, .max_weight = w});
  }

  static auto make_auth_data_delta(const AuthDataChangeArgs &args)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeAuthDataChange);
    // For simplicity, we'll create a basic auth data change
    // In a real scenario, this would be more complex
    delta.auth_data_change() = args;
    return delta;
  }

  static auto make_delete_delta()
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeDelete);
    return delta;
  }

  // check that the two addresses have the same balance,
  // and that that balance is less than or equal to
  // the initial balance
  void check_balances(ContractAddress contract_addr,
                      ContractAddress reference_addr, int128_t expected_balance)
  {
    // check that costs are reflected in the balance
    auto bal = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(contract_addr));
    auto bal_ref = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(reference_addr));
    ASSERT_TRUE(bal.has_value());
    ASSERT_TRUE(bal_ref.has_value());
    int128_t bal_int = from_xdr(bal.value().nnint());
    int128_t bal_ref_int = from_xdr(bal_ref.value().nnint());

    EXPECT_EQ(bal_int, bal_ref_int);
    EXPECT_EQ(bal_int, expected_balance);
  }

  const StorageCostModel *cost_model_;
};

TEST_P(DirectAuthDataTest, BasicAuthDataCreation)
{
  // Test basic creation of auth data
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create basic auth data
  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});

  auto tx1 = make_new_per_tx_db_proxy();
  // Note: This is a simplified test - actual auth data changes would use
  // the proper AuthDataChangeArgs structure
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(tx1.commit_transaction({}));
}

TEST_P(DirectAuthDataTest, AuthDataStorageCosts)
{
  // Test that auth data creation incurs proper storage costs
  auto contract_addr = make_contract_address(0);
  auto contract_addr_ref = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(contract_addr_ref, kInitialBalance);

  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back({0, make_pk_config(Weight{1})});

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_account_auth_data_addrkey(contract_addr),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());

  auth_data.auth_data_change_signers.push_back({1, make_pk_config(Weight{2})});

  tx1.add_delta(make_account_auth_data_addrkey(contract_addr_ref),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_2.has_value());
  ASSERT_TRUE(tx1.commit_transaction({}));

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_account_auth_data_addrkey(contract_addr_ref),
                make_auth_data_delta(auth_data),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(tx2.commit_transaction({}));

  advance_block();
  check_balances(contract_addr, contract_addr_ref,
                 kInitialBalance - (cost_model_->kBaseItemCost));
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectAuthDataTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
