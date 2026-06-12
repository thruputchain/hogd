#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "address_auth.hh"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/test_main.h"
#include "types.hh"

namespace hogd {

class GenesisBlockTest : public ::testing::Test {
protected:
  void SetUp() override {}

  // Mock storage write function that captures writes
  void mock_write_storage(StorageKey const &key, int128_t value)
  {
    storage_map_[key] = value;
  }

  void mock_write_contract(std::shared_ptr<const Contract> const &contract)
  {
    contract_map_[sha256(*contract)] = contract;
  }

  void mock_write_deployment(ContractAddress const &contract_address,
                             DeployedContractHash const &contract_hash)
  {
    deployment_map_[contract_address] = contract_hash;
  }

  int128_t mock_write_auth_data(ContractAddress const &contract_address,
                                AddressAuthData const &auth_data)
  {
    auth_data_map_[contract_address] = auth_data;
    return 100;
  }

  int128_t get_initial_balance()
  {
    auto key = StorageKey(StorageKeyType::FreeNativeTokenBalance);
    auto it = storage_map_.find(key);
    if (it == storage_map_.end()) {
      return 0;
    }
    return it->second;
  }

  // Helper to create a test genesis configuration
  static GenesisConfiguration create_test_genesis_configuration()
  {
    GenesisConfiguration genesis_configuration{};

    // Add a preset contract entry
    InitialConfigurationEntry preset_entry;

    // Create a test contract hash and nonce
    SHA256Hash contract_hash{};
    std::ranges::fill(contract_hash, 0x42);

    SHA256Hash nonce{};
    std::ranges::fill(nonce, 0x24);

    preset_entry.config.contract_hash = contract_hash;
    preset_entry.config.initial_nonce = nonce;
    preset_entry.config.entrypoint = ContractEntrypointID{};
    preset_entry.config.calldata = {0x01, 0x02, 0x03, 0x04};
    preset_entry.amount = to_xdr(int128_t{1000000}); // 1 million tokens

    genesis_configuration.initial_config.push_back(preset_entry);

    // Add another preset contract entry
    InitialConfigurationEntry preset_entry2;

    SHA256Hash contract_hash2{};
    std::ranges::fill(contract_hash2, 0x55);

    SHA256Hash nonce2{};
    std::ranges::fill(nonce2, 0x66);

    preset_entry2.config.contract_hash = contract_hash2;
    preset_entry2.config.initial_nonce = nonce2;
    preset_entry2.config.entrypoint = ContractEntrypointID{};
    preset_entry2.config.calldata = {0x05, 0x06, 0x07, 0x08};
    preset_entry2.amount = to_xdr(int128_t{500000}); // 500k tokens

    genesis_configuration.initial_config.push_back(preset_entry2);

    return genesis_configuration;
  }

  std::map<StorageKey, int128_t> storage_map_;
  std::map<SHA256Hash, std::shared_ptr<const Contract>> contract_map_;
  std::map<ContractAddress, DeployedContractHash> deployment_map_;
  std::map<ContractAddress, AddressAuthData> auth_data_map_;
};

TEST_F(GenesisBlockTest, LoadGenesisBlockSuccess)
{
  // Create test genesis block
  auto genesis_configuration = create_test_genesis_configuration();

  // Create loader and load genesis configuration
  GenesisConfigurationLoader loader(genesis_configuration);
  loader.load_genesis_configuration(
      [this](StorageKey const &key, int128_t value) -> int128_t {
        mock_write_storage(key, value);
        return 100;
      },
      [this](auto const &contract) { mock_write_contract(contract); },
      [this](ContractAddress const &contract_address,
             DeployedContractHash const &contract_hash) {
        mock_write_deployment(contract_address, contract_hash);
      },
      [this](ContractAddress const &contract_address,
             AddressAuthData const &auth_data) -> int128_t {
        return mock_write_auth_data(contract_address, auth_data);
      });

  EXPECT_EQ(storage_map_.size(), 3);
  EXPECT_EQ(contract_map_.size(), 2);
  EXPECT_EQ(deployment_map_.size(), 2);
  EXPECT_EQ(auth_data_map_.size(), 1);
  auto auth_it =
      auth_data_map_.find(GenesisConfigurationLoader::kDeployContractAddress);
  ASSERT_NE(auth_it, auth_data_map_.end());
  EXPECT_EQ(auth_it->second.required_weight, 0);
  EXPECT_EQ(get_initial_balance(),
            GenesisConfigurationLoader::initial_hog_token_supply() - 300);
}

TEST_F(GenesisBlockTest, CalculateStorageKeyPresetContract)
{
  SHA256Hash contract_hash{};
  std::ranges::fill(contract_hash, 0x42);

  SHA256Hash nonce{};
  std::ranges::fill(nonce, 0x24);

  ContractEntrypointID entrypoint{};
  std::ranges::fill(entrypoint, 0x00);

  std::vector<std::uint8_t> calldata = {0x01, 0x02, 0x03, 0x04};

  auto result =
      GenesisConfigurationLoader::calculate_storage_key(contract_hash, nonce,
                                                        entrypoint, calldata);

  // Verify the storage key is of the correct type
  EXPECT_EQ(result.key_type(), StorageKeyType::ContractData);

  // Verify the key is not all zeros
  bool has_non_zero = false;
  for (auto byte : result.contract_data_key()) {
    if (byte != 0) {
      has_non_zero = true;
      break;
    }
  }
  EXPECT_TRUE(has_non_zero);
}

TEST_F(GenesisBlockTest, EmptyGenesisBlock)
{
  // Create empty genesis block
  GenesisConfiguration genesis_configuration{};

  // Create loader and load genesis block
  GenesisConfigurationLoader loader(genesis_configuration);
  loader.load_genesis_configuration(
      [this](StorageKey const &key, int128_t value) -> int128_t {
        mock_write_storage(key, value);
        return 100;
      },
      [this](auto const &contract) { mock_write_contract(contract); },
      [this](ContractAddress const &contract_address,
             DeployedContractHash const &contract_hash) {
        mock_write_deployment(contract_address, contract_hash);
      },
      [this](ContractAddress const &contract_address,
             AddressAuthData const &auth_data) -> int128_t {
        return mock_write_auth_data(contract_address, auth_data);
      });

  EXPECT_EQ(storage_map_.size(), 1);
  EXPECT_EQ(contract_map_.size(), 2);
  EXPECT_EQ(deployment_map_.size(), 2);
  EXPECT_EQ(auth_data_map_.size(), 1);
  EXPECT_EQ(get_initial_balance(),
            GenesisConfigurationLoader::initial_hog_token_supply() - 100);
}

TEST_F(GenesisBlockTest, MultipleEntries)
{
  // Create test genesis block with multiple entries
  auto genesis_configuration = create_test_genesis_configuration();

  // Create loader and load genesis block
  GenesisConfigurationLoader loader(genesis_configuration);
  loader.load_genesis_configuration(
      [this](StorageKey const &key, int128_t value) -> int128_t {
        mock_write_storage(key, value);
        return 100;
      },
      [this](auto const &contract) { mock_write_contract(contract); },
      [this](ContractAddress const &contract_address,
             DeployedContractHash const &contract_hash) {
        mock_write_deployment(contract_address, contract_hash);
      },
      [this](ContractAddress const &contract_address,
             AddressAuthData const &auth_data) -> int128_t {
        return mock_write_auth_data(contract_address, auth_data);
      });

  EXPECT_EQ(storage_map_.size(), 3);
  EXPECT_EQ(contract_map_.size(), 2);
  EXPECT_EQ(deployment_map_.size(), 2);
  EXPECT_EQ(auth_data_map_.size(), 1);

  // Verify that different entries produce different keys
  auto keys = std::vector<StorageKey>();
  for (auto const &pair : storage_map_) {
    keys.push_back(pair.first);
  }

  EXPECT_NE(keys[0], keys[1]);
  EXPECT_EQ(get_initial_balance(),
            GenesisConfigurationLoader::initial_hog_token_supply() - 300);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  return hogd::run_hogexec_tests(argc, argv);
}
