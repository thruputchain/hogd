#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/network_config/config_upgrade.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

namespace {

ConfigUpgrade
upgrade_of(ConfigParamID id, uint32_t value)
{
  ConfigUpgrade upgrade(id);
  switch (id) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    upgrade.max_sha256_buffers() = value;
    break;
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    upgrade.max_cross_invoke_depth() = value;
    break;
  }
  return upgrade;
}

} // namespace

TEST(ConfigHashTest, CommitsToXdrPreimage)
{
  const StorageCostModel model = StorageCostModel::default_cost_model();
  const NetworkConfig network_config = stock_network_config();
  const ConfigHash expected{
      0x77, 0x22, 0xe0, 0x07, 0xb7, 0x38, 0xb5, 0xec, 0x0a, 0x56, 0x2f,
      0x1c, 0x9c, 0xaf, 0x59, 0x78, 0xfe, 0x0e, 0xb9, 0x35, 0xb9, 0xcb,
      0x70, 0x05, 0x56, 0xd6, 0x89, 0x2d, 0xde, 0x37, 0x48, 0xdc,
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

  const std::array<NetworkConfig, 2> variants = {
      apply_config_upgrade(
          stock_network_config(),
          upgrade_of(ConfigParamID::kConfigParamMaxSHA256Buffers,
                     stock_network_config().max_sha256_buffers + 1)),
      apply_config_upgrade(
          stock_network_config(),
          upgrade_of(ConfigParamID::kConfigParamMaxCrossInvokeDepth,
                     stock_network_config().max_cross_invoke_depth + 1)),
  };
  for (auto const &variant : variants) {
    EXPECT_NE(compute_config_hash(make_config_hash_preimage(model, variant)),
              original);
  }
}

} // namespace hogd
