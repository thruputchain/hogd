#include <gtest/gtest.h>

#include <limits>
#include <utility>

#include "address_auth.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class LoadgenTests : public ::testing::TestWithParam<TestDB>,
                     public NativeContractTestFramework {
public:
  LoadgenTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam(),
          false)
  {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_P(LoadgenTests, EmptyGenesisConfiguration)
{
  run_genesis_initialization(GenesisConfiguration{});

  auto bal = lookup_storage_item_from_db(make_free_native_token_balance_addrkey(
      GenesisConfigurationLoader::kDeployContractAddress));
  auto const *cost_model = get_default_cost_model();
  ASSERT_TRUE(bal.has_value());

  AddressAuthData deployer_auth_data{};
  deployer_auth_data.nonce_config.accepted_keys = 0;
  deployer_auth_data.nonce_config.required_weight = 0;
  deployer_auth_data.gas_permissions.push_back(
      GasPermissionConfig{.limit = std::numeric_limits<GasAmount>::max(),
                          .accepted_keys = 0,
                          .required_weight = 0});
  deployer_auth_data.last_written_height = 0;
  auto deployer_auth_item =
      StorageItemWrapper(std::move(deployer_auth_data), cost_model);

  ASSERT_EQ(from_xdr(bal.value().nnint()),
            GenesisConfigurationLoader::initial_hog_token_supply() -
                cost_model->get_balance_cost() -
                deployer_auth_item.storage_cost(cost_model));
}

TEST_P(LoadgenTests, OneInitialConfig)
{
  GenesisConfiguration genesis_configuration{};
  genesis_configuration.initial_config.emplace_back(InitialConfigurationEntry{
      .config = PresetDeploymentConfig{.initial_nonce = sha256(Bytestring{}),
                                       .contract_hash = sha256(Bytestring{}),
                                       .entrypoint = {},
                                       .calldata = Calldata{}},
      .amount = to_xdr(int128_t{1'000'000})});
  genesis_configuration.storage_cost_model = {
      .ext = ExtensionPoint(0),
      .base_data_item_cost = to_xdr(int128_t{12000}),
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = to_xdr(int128_t{12000}),
      .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
  };
  run_genesis_initialization(genesis_configuration);

  auto bal = lookup_storage_item_from_db(make_free_native_token_balance_addrkey(
      GenesisConfigurationLoader::kDeployContractAddress));
  ASSERT_TRUE(bal.has_value());
  EXPECT_LT(from_xdr(bal.value().nnint()),
            GenesisConfigurationLoader::initial_hog_token_supply());
  // TODO(geoff): assert balance = initial_supply - storage_cost(nnint)
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(LoadgenTests, LoadgenTests,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
