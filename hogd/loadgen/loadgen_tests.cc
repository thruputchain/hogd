#include <gtest/gtest.h>

#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_block.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"

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

TEST_P(LoadgenTests, EmptyGenesisBlock)
{
  run_genesis_initialization(GenesisBlock{});

  auto bal = lookup_storage_item_from_db(make_free_native_token_balance_addrkey(
      GenesisBlockLoader::kDeployContractAddress));
  ASSERT_TRUE(bal.has_value());
  ASSERT_EQ(from_xdr(bal.value().nnint()),
            GenesisBlockLoader::initial_hog_token_supply());
}

TEST_P(LoadgenTests, OneInitialConfig)
{
  GenesisBlock genesis_block;
  genesis_block.initial_config.emplace_back(InitialConfigurationEntry{
      .config = PresetDeploymentConfig{.initial_nonce = sha256(Bytestring{}),
                                       .contract_hash = sha256(Bytestring{}),
                                       .entrypoint = {},
                                       .calldata = Calldata{}},
      .amount = to_xdr(int128_t{1'000'000})});
  run_genesis_initialization(genesis_block);

  auto bal = lookup_storage_item_from_db(make_free_native_token_balance_addrkey(
      GenesisBlockLoader::kDeployContractAddress));
  ASSERT_TRUE(bal.has_value());
  EXPECT_LT(from_xdr(bal.value().nnint()),
            GenesisBlockLoader::initial_hog_token_supply());
  // TODO(geoff): assert balance = initial_supply - storage_cost(nnint)
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(LoadgenTests, LoadgenTests,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
