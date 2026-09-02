#include <gtest/gtest.h>

#include <array>

#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

TEST(ConfigHashTest, CommitsToXdrPreimage)
{
  const StorageCostModel model = StorageCostModel::default_cost_model();
  const NetworkConfig network_config = stock_network_config();
  const ConfigHash expected{
      0xb7, 0x55, 0xbb, 0x58, 0x61, 0x76, 0x2f, 0xce, 0x44, 0xb6, 0xcd,
      0xa1, 0x58, 0x30, 0xce, 0xee, 0x96, 0x38, 0x03, 0x28, 0x40, 0x7b,
      0x87, 0xaa, 0xf2, 0x16, 0x34, 0xb3, 0xbe, 0x70, 0x61, 0x6e,
  };
  EXPECT_EQ(compute_config_hash(
                make_config_hash_preimage(model, network_config)),
            expected);

  const ConfigHash original =
      compute_config_hash(make_config_hash_preimage(model, network_config));
  StorageCostModelXdr xdr = model.to_xdr();
  std::array<StorageCostModelXdr, 8> variants;
  variants.fill(xdr);
  variants[0].base_data_item_cost++;
  variants[1].bytestring_cost_per_byte++;
  variants[2].base_contract_deployment_cost++;
  variants[3].contract_deployment_cost_per_byte++;
  variants[4].auth_per_key_cost++;
  variants[5].rcq_cost_per_entry++;
  variants[6].replay_cache_cost_per_entry++;
  variants[7].balance_cost++;
  for (auto const &variant : variants) {
    EXPECT_NE(compute_config_hash(
                  make_config_hash_preimage(StorageCostModel(variant),
                                            network_config)),
              original);
  }
}

// Every configurable parameter has to move the hash, or a chain could change
// its execution rules without the header recording that it had.
TEST(ConfigHashTest, CommitsToEveryConfigParam)
{
  const StorageCostModel model = StorageCostModel::default_cost_model();
  const ConfigHash original = compute_config_hash(
      make_config_hash_preimage(model, stock_network_config()));

  std::array<NetworkConfig, 3> variants;
  variants.fill(stock_network_config());
  variants[0].max_sha256_buffers++;
  variants[1].max_cross_invoke_depth++;
  variants[2].max_new_contracts_per_tx++;
  for (auto const &variant : variants) {
    EXPECT_NE(compute_config_hash(make_config_hash_preimage(model, variant)),
              original);
  }
}

} // namespace hogd
