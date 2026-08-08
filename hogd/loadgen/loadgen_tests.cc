#include <gtest/gtest.h>

#include "address_auth.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class LoadgenTests : public ::testing::Test,
                     public NativeContractTestFramework {
public:
  LoadgenTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, false)
  {}

  static AddressAuthData preset_deployer_auth_data()
  {
    AddressAuthData deployer_auth_data{};
    deployer_auth_data.required_weight = 0;
    deployer_auth_data.last_written_height = 0;
    return deployer_auth_data;
  }
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(LoadgenTests, EmptyGenesisConfiguration)
{
  auto cost_model = StorageCostModel::default_cost_model();
  run_genesis_initialization(
      GenesisConfiguration{
          .storage_cost_model = cost_model.to_xdr(),
          .default_wallet =
              GenesisConfigurationLoader::native_default_wallet_hash()});

  auto bal = lookup_storage_item_from_db(make_free_native_token_balance_addrkey(
      GenesisConfigurationLoader::kDeployContractAddress));
  ASSERT_TRUE(bal.has_value());

  auto deployer_auth_item =
      StorageItemWrapper(preset_deployer_auth_data(), &cost_model);

  ASSERT_EQ(from_xdr(bal.value().nnint()),
            GenesisConfigurationLoader::initial_hog_token_supply() -
                cost_model.get_balance_cost() -
                deployer_auth_item.storage_cost(&cost_model));

  auto auth_data = lookup_storage_item_from_db(make_address_auth_data_addrkey(
      GenesisConfigurationLoader::kDeployContractAddress));
  ASSERT_TRUE(auth_data.has_value());
  EXPECT_EQ(auth_data.value().auth_data(), preset_deployer_auth_data());
}

TEST_F(LoadgenTests, OneInitialConfig)
{
  auto cost_model = StorageCostModel::default_cost_model();
  GenesisConfiguration genesis_configuration{
      .storage_cost_model = cost_model.to_xdr(),
      .default_wallet =
          GenesisConfigurationLoader::native_default_wallet_hash()};
  genesis_configuration.initial_config.emplace_back(InitialConfigurationEntry{
      .config = PresetDeploymentConfig{.initial_nonce = sha256(Bytestring{}),
                                       .contract_hash = sha256(Bytestring{}),
                                       .entrypoint = {},
                                       .calldata = Calldata{}},
      .amount = to_xdr(int128_t{1'000'000})});
  run_genesis_initialization(genesis_configuration);

  auto deployer_auth_item =
      StorageItemWrapper(preset_deployer_auth_data(), &cost_model);

  auto bal = lookup_storage_item_from_db(make_free_native_token_balance_addrkey(
      GenesisConfigurationLoader::kDeployContractAddress));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()),
            GenesisConfigurationLoader::initial_hog_token_supply() -
                cost_model.get_balance_cost() -
                deployer_auth_item.storage_cost(&cost_model) -
                cost_model.get_nnint_cost());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
