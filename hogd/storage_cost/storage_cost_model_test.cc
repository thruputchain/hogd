#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
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
      .base_data_item_cost = ::hogd::to_xdr(int128_t{42}),
      .bytestring_cost_per_byte = 7,
      .base_contract_deployment_cost = ::hogd::to_xdr(int128_t{100}),
      .contract_deployment_cost_per_byte = ::hogd::to_xdr(int128_t{3}),
      .auth_per_key_cost = ::hogd::to_xdr(int128_t{55}),
      .rcq_cost_per_entry = ::hogd::to_xdr(int128_t{11}),
      .rcq_cost_per_block = ::hogd::to_xdr(int128_t{13}),
      .replay_cache_cost_per_entry = ::hogd::to_xdr(int128_t{17}),
      .balance_cost = ::hogd::to_xdr(int128_t{9999}),
  };

  StorageCostModel model(original);
  auto roundtripped = model.to_xdr();

  EXPECT_EQ(original, roundtripped);
}

} // namespace hogd
