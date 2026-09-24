#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <utility>

#include "auth_token.hh"
#include "contract.hh"
#include "hog_exec_constants.hh"
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
  const DeployedContractHash wallet = native_default_wallet_hash();
  // Moves whenever the stock configuration changes, which is the point: it
  // pins the preimage so a change to it is deliberate.
  const ConfigHash expected{
      0xdd, 0xc3, 0x86, 0x5b, 0xbd, 0xa3, 0xbf, 0xbb, 0x46, 0x65, 0xa6,
      0xef, 0x2d, 0x04, 0xb1, 0xb8, 0x82, 0xcb, 0x43, 0x0f, 0xa6, 0x5a,
      0x0e, 0xa8, 0xc1, 0x31, 0x01, 0x32, 0x5b, 0x76, 0x37, 0xd6,
  };
  EXPECT_EQ(compute_config_hash(
                make_config_hash_preimage(model, network_config, wallet)),
            expected);

  const ConfigHash original = compute_config_hash(
      make_config_hash_preimage(model, network_config, wallet));
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
                                            network_config, wallet)),
              original);
  }
}

// Every configurable parameter has to move the hash, or a chain could change
// its execution rules without the header recording that it had.
TEST(ConfigHashTest, CommitsToEveryConfigParam)
{
  const StorageCostModel model = StorageCostModel::default_cost_model();
  const ConfigHash original = compute_config_hash(
      make_config_hash_preimage(model, stock_network_config(),
                                native_default_wallet_hash()));

  std::array<NetworkConfig, 10> variants;
  variants.fill(stock_network_config());
  const NetworkConfigParams stock(stock_network_config().v0());
  NetworkConfigParams::set_param(variants[0],
                                 ConfigParamID::kConfigParamMaxSHA256Buffers,
                                 stock.max_sha256_buffers + 1);
  NetworkConfigParams::set_param(variants[1],
                                 ConfigParamID::kConfigParamMaxCrossInvokeDepth,
                                 stock.max_cross_invoke_depth + 1);
  NetworkConfigParams::set_param(
      variants[2], ConfigParamID::kConfigParamMaxNewContractsPerTx,
      stock.max_new_contracts_per_tx + 1);
  NetworkConfigParams::set_param(variants[3],
                                 ConfigParamID::kConfigParamMaxBytestringLen,
                                 stock.max_bytestring_len + 1);
  NetworkConfigParams::set_param(
      variants[4], ConfigParamID::kConfigParamMaxContractSizeBytes,
      stock.max_contract_size_bytes + 1);
  NetworkConfigParams::set_param(variants[5],
                                 ConfigParamID::kConfigParamMaxSavepointDepth,
                                 stock.max_savepoint_depth + 1);
  NetworkConfigParams::set_param(
      variants[6], ConfigParamID::kConfigParamMaxSHA256BufferBytes,
      stock.max_sha256_buffer_bytes + 1);
  NetworkConfigParams::set_param(variants[7],
                                 ConfigParamID::kConfigParamMaxAuthTokenDepth,
                                 stock.max_auth_token_depth + 1);
  NetworkConfigParams::set_param(variants[8],
                                 ConfigParamID::kConfigParamMaxRv32CodeBytes,
                                 stock.max_rv32_code_bytes + 1);
  NetworkConfigParams::set_param(variants[9],
                                 ConfigParamID::kConfigParamMaxRv32MemoryBytes,
                                 stock.max_rv32_memory_bytes + 1);
  for (auto const &variant : variants) {
    EXPECT_NE(compute_config_hash(
                  make_config_hash_preimage(model, variant,
                                            native_default_wallet_hash())),
              original);
  }
}

// The default wallet selects executing code, and a node's copy of it rides in
// database metadata the state hash does not cover, so the header has to be
// what pins it. See groundhogchain/hogd#3926.
TEST(ConfigHashTest, CommitsToDefaultWallet)
{
  const StorageCostModel model = StorageCostModel::default_cost_model();
  const NetworkConfig network_config = stock_network_config();
  const DeployedContractHash wallet = native_default_wallet_hash();
  const ConfigHash original = compute_config_hash(
      make_config_hash_preimage(model, network_config, wallet));

  DeployedContractHash other_hash = wallet;
  other_hash.hash[0] ^= 1;
  DeployedContractHash other_size = wallet;
  other_size.contract_size++;
  DeployedContractHash other_type = wallet;
  other_type.contract_type = ContractType::kRv32;
  ASSERT_NE(other_type.contract_type, wallet.contract_type);

  for (auto const &variant : {other_hash, other_size, other_type}) {
    EXPECT_NE(compute_config_hash(
                  make_config_hash_preimage(model, network_config, variant)),
              original);
  }
}

// The stock configuration is in the only form that may be hashed, and
// set_param keeps it that way rather than appending out of order.
TEST(ConfigHashTest, StockConfigIsCanonical)
{
  NetworkConfig config = stock_network_config();
  ASSERT_EQ(config.v(), 0);
  EXPECT_TRUE(NetworkConfigParams::is_canonical(config.v0()));
  EXPECT_TRUE(config_hashable(config));

  // Setting a parameter already stated replaces it in place; setting one out
  // of id order still lands in order.
  NetworkConfigParams::set_param(
      config, ConfigParamID::kConfigParamMaxNewContractsPerTx, 8);
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxSHA256Buffers,
                                 4);
  EXPECT_TRUE(NetworkConfigParams::is_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), 10U);
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_new_contracts_per_tx, 8U);
  EXPECT_EQ(params.max_sha256_buffers, 4U);
}

// Stating the same parameters lands the same list whatever order they are set
// in, so the order a builder happens to use cannot move the hash. Every
// permutation, because the interesting cases are where an earlier set_param
// default-fills a slot a later one then replaces.
TEST(ConfigHashTest, SetParamOrderIndependent)
{
  std::array<std::pair<ConfigParamID, uint32_t>, 5> stated{{
      {ConfigParamID::kConfigParamMaxSHA256Buffers, 5},
      {ConfigParamID::kConfigParamMaxCrossInvokeDepth, 33},
      {ConfigParamID::kConfigParamMaxNewContractsPerTx, 7},
      {ConfigParamID::kConfigParamMaxBytestringLen, 4096},
      {ConfigParamID::kConfigParamMaxContractSizeBytes,
       static_cast<uint32_t>(1_MiB)},
  }};

  NetworkConfig reference;
  reference.v(0);
  for (auto const &[id, value] : stated) {
    NetworkConfigParams::set_param(reference, id, value);
  }
  ASSERT_TRUE(NetworkConfigParams::is_canonical(reference.v0()));

  std::ranges::sort(stated);
  do {
    NetworkConfig config;
    config.v(0);
    for (auto const &[id, value] : stated) {
      NetworkConfigParams::set_param(config, id, value);
    }
    EXPECT_EQ(config, reference);
  } while (std::ranges::next_permutation(stated).found);
}

// A configuration predating the bytestring parameter reads as the limit every
// chain ran under before it existed, so adding the parameter changed no
// chain's behaviour.
TEST(ConfigHashTest, ShortListYieldsBytestringDefault)
{
  NetworkConfig config;
  config.v(0);
  NetworkConfigParams::set_param(
      config, ConfigParamID::kConfigParamMaxNewContractsPerTx, 4);
  ASSERT_EQ(config.v0().params.size(), 3U);
  EXPECT_EQ(NetworkConfigParams(config.v0()).max_bytestring_len,
            static_cast<uint32_t>(kDefaultMaxBytestringLen));

  // Stating it extends the prefix by one and takes effect.
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxBytestringLen,
                                 4096);
  EXPECT_TRUE(NetworkConfigParams::is_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), 4U);
  EXPECT_EQ(NetworkConfigParams(config.v0()).max_bytestring_len, 4096U);
}

// A list stopping at the bytestring limit uses the contract-size default.
TEST(ConfigHashTest, ShortListYieldsContractSizeDefault)
{
  NetworkConfig config;
  config.v(0);
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxBytestringLen,
                                 4096);
  ASSERT_EQ(config.v0().params.size(), 4U);
  EXPECT_EQ(NetworkConfigParams(config.v0()).max_contract_size_bytes,
            static_cast<uint32_t>(kDefaultMaxContractSizeBytes));
  EXPECT_EQ(NetworkConfigParams(config.v0()).max_contract_size_bytes, 512_KiB);

  // Stating it extends the prefix by one and takes effect, above the default
  // as well as below it.
  NetworkConfigParams::set_param(
      config, ConfigParamID::kConfigParamMaxContractSizeBytes,
      static_cast<uint32_t>(1_MiB));
  EXPECT_TRUE(NetworkConfigParams::is_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), 5U);
  EXPECT_EQ(NetworkConfigParams(config.v0()).max_contract_size_bytes, 1_MiB);
}

// And for the execution limits added after it: a list stopping at the
// contract size reads as the protocol defaults, and stating one extends the
// prefix, default-filling the slots on the way.
TEST(ConfigHashTest, ShortListYieldsExecutionLimitDefaults)
{
  NetworkConfig config;
  config.v(0);
  NetworkConfigParams::set_param(
      config, ConfigParamID::kConfigParamMaxContractSizeBytes,
      static_cast<uint32_t>(256_KiB));
  ASSERT_EQ(config.v0().params.size(), 5U);
  const NetworkConfigParams defaults(config.v0());
  EXPECT_EQ(defaults.max_savepoint_depth,
            static_cast<uint32_t>(kMaxSavepointDepth));
  EXPECT_EQ(defaults.max_sha256_buffer_bytes,
            static_cast<uint32_t>(kSHA256BufferMaxSize));
  EXPECT_EQ(defaults.max_auth_token_depth,
            static_cast<uint32_t>(kAuthTokenMaxDepth));
  EXPECT_EQ(defaults.max_rv32_code_bytes,
            static_cast<uint32_t>(kDefaultMaxRv32CodeBytes));
  EXPECT_EQ(defaults.max_rv32_code_bytes, 256_KiB);
  EXPECT_EQ(defaults.max_rv32_memory_bytes,
            static_cast<uint32_t>(kDefaultMaxRv32MemoryBytes));

  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxRv32MemoryBytes,
                                 static_cast<uint32_t>(1_MiB));
  EXPECT_TRUE(NetworkConfigParams::is_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), 10U);
  const NetworkConfigParams stated(config.v0());
  EXPECT_EQ(stated.max_rv32_memory_bytes, 1_MiB);
  EXPECT_EQ(stated.max_savepoint_depth,
            static_cast<uint32_t>(kMaxSavepointDepth));
  EXPECT_EQ(stated.max_rv32_code_bytes,
            static_cast<uint32_t>(kDefaultMaxRv32CodeBytes));
}

// A repeated or misordered list is refused, because it would give one
// configuration more than one hash.
TEST(ConfigHashTest, RejectsNonCanonicalParamList)
{
  NetworkConfigV0 repeated;
  repeated.params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxSHA256Buffers, 8));
  repeated.params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxSHA256Buffers, 9));
  EXPECT_FALSE(NetworkConfigParams::is_canonical(repeated));

  NetworkConfigV0 descending;
  descending.params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxCrossInvokeDepth, 256));
  descending.params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxSHA256Buffers, 8));
  EXPECT_FALSE(NetworkConfigParams::is_canonical(descending));

  // A gap is refused even though the ids ascend: the canonical form is a
  // prefix, so being absent can only mean being past the end.
  NetworkConfigV0 gap;
  gap.params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxSHA256Buffers, 8));
  gap.params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxNewContractsPerTx, 4));
  EXPECT_FALSE(NetworkConfigParams::is_canonical(gap));

  // Nor may the list start past the first parameter.
  NetworkConfigV0 offset;
  offset.params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxCrossInvokeDepth, 256));
  EXPECT_FALSE(NetworkConfigParams::is_canonical(offset));

  NetworkConfigV0 empty;
  EXPECT_TRUE(NetworkConfigParams::is_canonical(empty));

  // Setting a later parameter fills the ones before it rather than leaving a
  // gap behind.
  NetworkConfig fresh;
  fresh.v(0);
  NetworkConfigParams::set_param(
      fresh, ConfigParamID::kConfigParamMaxNewContractsPerTx, 4);
  EXPECT_TRUE(NetworkConfigParams::is_canonical(fresh.v0()));
  EXPECT_EQ(fresh.v0().params.size(), 3U);
}

// Values chosen off the defaults so a misindexed read shows.
TEST(ConfigHashTest, ReadsEachParamFromItsOwnIndex)
{
  NetworkConfig prefix;
  prefix.v(0);
  prefix.v0().params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxSHA256Buffers, 3));
  prefix.v0().params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxCrossInvokeDepth, 7));
  ASSERT_TRUE(NetworkConfigParams::is_canonical(prefix.v0()));
  const NetworkConfigParams prefix_params(prefix.v0());
  EXPECT_EQ(prefix_params.max_sha256_buffers, 3U);
  EXPECT_EQ(prefix_params.max_cross_invoke_depth, 7U);
  // Past the end of a shorter list, so the default stands.
  EXPECT_EQ(prefix_params.max_new_contracts_per_tx,
            kDefaultMaxNewContractsPerTx);

  NetworkConfig full = prefix;
  full.v0().params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxNewContractsPerTx, 9));
  ASSERT_TRUE(NetworkConfigParams::is_canonical(full.v0()));
  EXPECT_EQ(NetworkConfigParams(full.v0()).max_new_contracts_per_tx, 9U);
}

// A configuration stating fewer parameters than this build knows runs under
// the defaults for the rest.
TEST(ConfigHashTest, ShortListTakesDefaults)
{
  NetworkConfig config;
  config.v(0);
  config.v0().params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxSHA256Buffers, 16));
  config.v0().params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxCrossInvokeDepth, 64));

  const NetworkConfigParams stated(config.v0());
  EXPECT_EQ(stated.max_sha256_buffers, 16U);
  EXPECT_EQ(stated.max_cross_invoke_depth, 64U);
  EXPECT_EQ(stated.max_new_contracts_per_tx, kDefaultMaxNewContractsPerTx);

  // Setting one carries the others' stated values rather than defaulting them.
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxSHA256Buffers,
                                 2);
  EXPECT_TRUE(NetworkConfigParams::is_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), 2U);
  const NetworkConfigParams updated(config.v0());
  EXPECT_EQ(updated.max_sha256_buffers, 2U);
  EXPECT_EQ(updated.max_cross_invoke_depth, 64U);
  EXPECT_NE(updated.max_cross_invoke_depth, kDefaultMaxCrossInvokeDepth);
  EXPECT_EQ(updated.max_new_contracts_per_tx, kDefaultMaxNewContractsPerTx);
}

} // namespace hogd
