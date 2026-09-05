#include <gtest/gtest.h>

#include <array>

#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

TEST(ConfigHashTest, CommitsToXdrPreimage)
{
  const StorageCostModel model = StorageCostModel::default_cost_model();
  const NetworkConfig network_config = stock_network_config();
  // Moves whenever the stock configuration changes, which is the point: it
  // pins the preimage so a change to it is deliberate.
  const ConfigHash expected{
      0x53, 0xa8, 0x94, 0x0a, 0x37, 0x53, 0xa4, 0x41, 0xef, 0x9d, 0x17,
      0xf7, 0xb8, 0xc1, 0x0a, 0xcc, 0xaa, 0x1d, 0xac, 0x02, 0xdb, 0x73,
      0x9a, 0x7b, 0x79, 0xea, 0x07, 0xf8, 0x9f, 0xe6, 0xb7, 0x7d,
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

  std::array<NetworkConfig, 4> variants;
  variants.fill(stock_network_config());
  set_config_param(variants[0], ConfigParamID::kConfigParamMaxSHA256Buffers,
                   max_sha256_buffers(variants[0]) + 1);
  set_config_param(variants[1], ConfigParamID::kConfigParamMaxCrossInvokeDepth,
                   max_cross_invoke_depth(variants[1]) + 1);
  set_config_param(variants[2], ConfigParamID::kConfigParamMaxNewContractsPerTx,
                   max_new_contracts_per_tx(variants[2]) + 1);
  set_config_param(variants[3], ConfigParamID::kConfigParamMaxBytestringLen,
                   max_bytestring_len(variants[3]) + 1);
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
  ASSERT_EQ(config.v(), 0);
  EXPECT_TRUE(config_params_canonical(config.v0()));
  EXPECT_TRUE(config_hashable(config));

  // Setting a parameter already stated replaces it in place; setting one out
  // of id order still lands in order.
  set_config_param(config, ConfigParamID::kConfigParamMaxNewContractsPerTx, 8);
  set_config_param(config, ConfigParamID::kConfigParamMaxSHA256Buffers, 4);
  EXPECT_TRUE(config_params_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), 4U);
  EXPECT_EQ(max_new_contracts_per_tx(config), 8U);
  EXPECT_EQ(max_sha256_buffers(config), 4U);
}

// A configuration predating the bytestring parameter reads as the limit every
// chain ran under before it existed, so adding the parameter changed no
// chain's behaviour.
TEST(ConfigHashTest, ShortListYieldsBytestringDefault)
{
  NetworkConfig config;
  config.v(0);
  set_config_param(config, ConfigParamID::kConfigParamMaxNewContractsPerTx, 4);
  ASSERT_EQ(config.v0().params.size(), 3U);
  EXPECT_EQ(max_bytestring_len(config),
            static_cast<uint32_t>(kDefaultMaxBytestringLen));

  // Stating it extends the prefix by one and takes effect.
  set_config_param(config, ConfigParamID::kConfigParamMaxBytestringLen, 4096);
  EXPECT_TRUE(config_params_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), 4U);
  EXPECT_EQ(max_bytestring_len(config), 4096U);
}

// A repeated or misordered list is refused, because it would give one
// configuration more than one hash.
TEST(ConfigHashTest, RejectsNonCanonicalParamList)
{
  NetworkConfigV0 repeated;
  repeated.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 8));
  repeated.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 9));
  EXPECT_FALSE(config_params_canonical(repeated));

  NetworkConfigV0 descending;
  descending.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxCrossInvokeDepth, 256));
  descending.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 8));
  EXPECT_FALSE(config_params_canonical(descending));

  // A gap is refused even though the ids ascend: the canonical form is a
  // prefix, so being absent can only mean being past the end.
  NetworkConfigV0 gap;
  gap.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 8));
  gap.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxNewContractsPerTx, 4));
  EXPECT_FALSE(config_params_canonical(gap));

  // Nor may the list start past the first parameter.
  NetworkConfigV0 offset;
  offset.params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxCrossInvokeDepth, 256));
  EXPECT_FALSE(config_params_canonical(offset));

  NetworkConfigV0 empty;
  EXPECT_TRUE(config_params_canonical(empty));

  // Setting a later parameter fills the ones before it rather than leaving a
  // gap behind.
  NetworkConfig fresh;
  fresh.v(0);
  set_config_param(fresh, ConfigParamID::kConfigParamMaxNewContractsPerTx, 4);
  EXPECT_TRUE(config_params_canonical(fresh.v0()));
  EXPECT_EQ(fresh.v0().params.size(), 3U);
}

// Values chosen off the defaults so a misindexed read shows.
TEST(ConfigHashTest, ReadsEachParamFromItsOwnIndex)
{
  NetworkConfig prefix;
  prefix.v(0);
  prefix.v0().params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 3));
  prefix.v0().params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxCrossInvokeDepth, 7));
  ASSERT_TRUE(config_params_canonical(prefix.v0()));
  EXPECT_EQ(max_sha256_buffers(prefix), 3U);
  EXPECT_EQ(max_cross_invoke_depth(prefix), 7U);
  // Past the end of a shorter list, so the default stands.
  EXPECT_EQ(max_new_contracts_per_tx(prefix), kDefaultMaxNewContractsPerTx);

  NetworkConfig full = prefix;
  full.v0().params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxNewContractsPerTx, 9));
  ASSERT_TRUE(config_params_canonical(full.v0()));
  EXPECT_EQ(max_new_contracts_per_tx(full), 9U);
}

// A configuration stating fewer parameters than this build knows runs under
// the defaults for the rest.
TEST(ConfigHashTest, ShortListTakesDefaults)
{
  NetworkConfig config;
  config.v(0);
  config.v0().params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxSHA256Buffers, 16));
  config.v0().params.push_back(
      detail::make_param(ConfigParamID::kConfigParamMaxCrossInvokeDepth, 64));

  EXPECT_EQ(max_sha256_buffers(config), 16U);
  EXPECT_EQ(max_cross_invoke_depth(config), 64U);
  EXPECT_EQ(max_new_contracts_per_tx(config), kDefaultMaxNewContractsPerTx);

  // Setting one carries the others' stated values rather than defaulting them.
  set_config_param(config, ConfigParamID::kConfigParamMaxSHA256Buffers, 2);
  EXPECT_TRUE(config_params_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), 2U);
  EXPECT_EQ(max_sha256_buffers(config), 2U);
  EXPECT_EQ(max_cross_invoke_depth(config), 64U);
  EXPECT_NE(max_cross_invoke_depth(config), kDefaultMaxCrossInvokeDepth);
  EXPECT_EQ(max_new_contracts_per_tx(config), kDefaultMaxNewContractsPerTx);
}

} // namespace hogd
