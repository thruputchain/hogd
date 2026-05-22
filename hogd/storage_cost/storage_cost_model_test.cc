#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

// Roundtrip: construct a StorageCostModel from an XDR, convert back to XDR,
// and verify the XDR structs are equal.
TEST(StorageCostModelTest, RoundtripDefaultCostModel)
{
  auto model = StorageCostModel::default_cost_model();
  auto xdr = model.to_xdr();

  StorageCostModel reconstructed(xdr);
  auto xdr2 = reconstructed.to_xdr();

  EXPECT_EQ(xdr, xdr2);
}

// Roundtrip: construct from custom XDR values, convert back, and verify
// field-level equality.
TEST(StorageCostModelTest, RoundtripCustomValues)
{
  StorageCostModelXdr original{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 42,
      .bytestring_cost_per_byte = 7,
      .base_contract_deployment_cost = 100,
      .contract_deployment_cost_per_byte = 3,
      .auth_per_key_cost = 55,
      .rcq_cost_per_entry = 11,
      .replay_cache_cost_per_entry = 17,
      .balance_cost = 9999,
  };

  StorageCostModel model(original);
  auto roundtripped = model.to_xdr();

  EXPECT_EQ(original, roundtripped);
}

// Verify that get_bytestring_cost does not truncate for large bytestring_len
// values. With kBytestringCostPerByte = 10, a bytestring_len near the 64-bit
// max would overflow in 64-bit arithmetic but should be correct in 128-bit.
TEST(StorageCostModelTest, GetBytestringCostLargeLen)
{
  auto model = StorageCostModel::default_cost_model();
  // default_cost_model has kBaseItemCost = 12000, kBytestringCostPerByte = 10

  // UINT64_MAX / 10 = 1844674407370955161, so any value larger than that
  // multiplied by 10 will overflow uint64_t.
  // Use 2000000000000000000 (2e18) * 10 = 2e19 which overflows uint64_t
  // (max ~1.84e19).
  auto large_len = size_t{2'000'000'000'000'000'000ULL};
  int128_t expected = int128_t{12000} + (int128_t{large_len} * int128_t{10});
  int128_t actual = model.get_bytestring_cost(large_len);
  EXPECT_EQ(actual, expected);
}

TEST(StorageCostModelTest, ContractDeploymentCostIncludesBase)
{
  StorageCostModelXdr xdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 5000,
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = 12000,
      .contract_deployment_cost_per_byte = 10,
      .auth_per_key_cost = 1111,
      .rcq_cost_per_entry = 8,
      .replay_cache_cost_per_entry = 7,
      .balance_cost = 1234,
  };
  StorageCostModel model(xdr);

  DeployedContractHash wasm_hash{.hash = {},
                                 .contract_size = 100,
                                 .contract_type = ContractType::kWasmi};
  EXPECT_EQ(model.get_deployment_cost(wasm_hash), int128_t{13000});

  wasm_hash.contract_size = 0;
  EXPECT_EQ(model.get_deployment_cost(wasm_hash), int128_t{12000});

  DeployedContractHash native_hash{.hash = {},
                                   .contract_size = 0,
                                   .contract_type = ContractType::kNative};
  EXPECT_EQ(model.get_deployment_cost(native_hash), int128_t{0});

  DeployedContractHash mock_hash{.hash = {},
                                 .contract_size = 100,
                                 .contract_type = ContractType::kMockTest};
  EXPECT_EQ(model.get_deployment_cost(mock_hash), int128_t{0});
}

} // namespace hogd
