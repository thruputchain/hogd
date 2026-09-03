#include <gtest/gtest.h>

#include <array>

#include "hogd/base/network_config_params.h"
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
      0x68, 0x92, 0xc2, 0xa1, 0x1c, 0x26, 0x06, 0xcc, 0x5b, 0x1c, 0xab,
      0x17, 0x5e, 0x1b, 0x8c, 0xf2, 0x6b, 0x17, 0x6a, 0x40, 0xc6, 0xc7,
      0x74, 0xef, 0x13, 0x0f, 0x7c, 0xd4, 0x3d, 0x39, 0x3d, 0x25,
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
  set_config_param(variants[0], ConfigParamID::kConfigParamMaxSHA256Buffers,
                   max_sha256_buffers(variants[0]) + 1);
  set_config_param(variants[1], ConfigParamID::kConfigParamMaxCrossInvokeDepth,
                   max_cross_invoke_depth(variants[1]) + 1);
  set_config_param(variants[2], ConfigParamID::kConfigParamMaxNewContractsPerTx,
                   max_new_contracts_per_tx(variants[2]) + 1);
  for (auto const &variant : variants) {
    EXPECT_NE(compute_config_hash(make_config_hash_preimage(model, variant)),
              original);
  }
}

// The stock configuration is in the only form that may be hashed, and
// set_config_param keeps it that way rather than appending out of order.
TEST(ConfigHashTest, StockConfigIsCanonical)
{
  NetworkConfig config = stock_network_config();
  ASSERT_EQ(config.v(), 1);
  EXPECT_TRUE(config_params_canonical(config.v1()));
  EXPECT_TRUE(config_hashable(config));

  // Setting a parameter already stated replaces it in place; setting one out
  // of id order still lands in order.
  set_config_param(config, ConfigParamID::kConfigParamMaxNewContractsPerTx, 8);
  set_config_param(config, ConfigParamID::kConfigParamMaxSHA256Buffers, 4);
  EXPECT_TRUE(config_params_canonical(config.v1()));
  EXPECT_EQ(config.v1().params.size(), 3U);
  EXPECT_EQ(max_new_contracts_per_tx(config), 8U);
  EXPECT_EQ(max_sha256_buffers(config), 4U);
}

// A repeated or misordered list is refused, because it would give one
// configuration more than one hash.
TEST(ConfigHashTest, RejectsNonCanonicalParamList)
{
  NetworkConfigV1 repeated;
  repeated.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 8));
  repeated.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 9));
  EXPECT_FALSE(config_params_canonical(repeated));

  NetworkConfigV1 descending;
  descending.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxCrossInvokeDepth, 256));
  descending.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 8));
  EXPECT_FALSE(config_params_canonical(descending));

  NetworkConfigV1 empty;
  EXPECT_TRUE(config_params_canonical(empty));
}

// A chain predating a parameter runs under the node's default for it rather
// than under no bound at all.
TEST(ConfigHashTest, LegacyConfigTakesDefaults)
{
  NetworkConfig legacy;
  legacy.v(0);
  legacy.v0().max_sha256_buffers = 16;
  legacy.v0().max_cross_invoke_depth = 64;

  EXPECT_EQ(max_sha256_buffers(legacy), 16U);
  EXPECT_EQ(max_cross_invoke_depth(legacy), 64U);
  EXPECT_EQ(max_new_contracts_per_tx(legacy), kDefaultMaxNewContractsPerTx);

  // Promotion carries the values in force, including the one v0 never stated.
  set_config_param(legacy, ConfigParamID::kConfigParamMaxSHA256Buffers, 2);
  ASSERT_EQ(legacy.v(), 1);
  EXPECT_TRUE(config_params_canonical(legacy.v1()));
  EXPECT_EQ(max_sha256_buffers(legacy), 2U);
  EXPECT_EQ(max_cross_invoke_depth(legacy), 64U);
  EXPECT_EQ(max_new_contracts_per_tx(legacy), kDefaultMaxNewContractsPerTx);
}

} // namespace hogd
