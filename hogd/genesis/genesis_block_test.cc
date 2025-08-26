#include "hogd/genesis/genesis_block.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "types.hh"
#include "gtest/gtest.h"

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
                             SHA256Hash const &contract_hash)
  {
    deployment_map_[contract_address] = contract_hash;
  }

  // Helper to create a test genesis block
  static GenesisBlock create_test_genesis_block()
  {
    GenesisBlock genesis_block;

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

    genesis_block.initial_config.push_back(preset_entry);

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

    genesis_block.initial_config.push_back(preset_entry2);

    return genesis_block;
  }

  std::map<StorageKey, int128_t> storage_map_;
  std::map<SHA256Hash, std::shared_ptr<const Contract>> contract_map_;
  std::map<ContractAddress, SHA256Hash> deployment_map_;
};

TEST_F(GenesisBlockTest, LoadGenesisBlockSuccess)
{
  // Create test genesis block
  auto genesis_block = create_test_genesis_block();

  // Create loader and load genesis block
  GenesisBlockLoader loader(genesis_block);
  loader.load_genesis_block(
      [this](StorageKey const &key, int128_t value) {
        mock_write_storage(key, value);
      },
      [this](auto const &contract) { mock_write_contract(contract); },
      [this](ContractAddress const &contract_address,
             SHA256Hash const &contract_hash) {
        mock_write_deployment(contract_address, contract_hash);
      });

  EXPECT_EQ(storage_map_.size(), 3);
  EXPECT_EQ(contract_map_.size(), 2);
  EXPECT_EQ(deployment_map_.size(), 2);
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

  auto result = GenesisBlockLoader::calculate_storage_key(contract_hash, nonce,
                                                          entrypoint, calldata);

  // Verify the storage key is of the correct type
  EXPECT_EQ(result.key_type(), StorageKeyType::kStorageKeyTypeContractData);

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
  GenesisBlock genesis_block;

  // Create loader and load genesis block
  GenesisBlockLoader loader(genesis_block);
  loader.load_genesis_block(
      [this](StorageKey const &key, int128_t value) {
        mock_write_storage(key, value);
      },
      [this](auto const &contract) { mock_write_contract(contract); },
      [this](ContractAddress const &contract_address,
             SHA256Hash const &contract_hash) {
        mock_write_deployment(contract_address, contract_hash);
      });

  EXPECT_EQ(storage_map_.size(), 1);
  EXPECT_EQ(contract_map_.size(), 2);
  EXPECT_EQ(deployment_map_.size(), 2);
}

TEST_F(GenesisBlockTest, MultipleEntries)
{
  // Create test genesis block with multiple entries
  auto genesis_block = create_test_genesis_block();

  // Create loader and load genesis block
  GenesisBlockLoader loader(genesis_block);
  loader.load_genesis_block(
      [this](StorageKey const &key, int128_t value) {
        mock_write_storage(key, value);
      },
      [this](auto const &contract) { mock_write_contract(contract); },
      [this](ContractAddress const &contract_address,
             SHA256Hash const &contract_hash) {
        mock_write_deployment(contract_address, contract_hash);
      });

  EXPECT_EQ(storage_map_.size(), 3);
  EXPECT_EQ(contract_map_.size(), 2);
  EXPECT_EQ(deployment_map_.size(), 2);

  // Verify that different entries produce different keys
  auto keys = std::vector<StorageKey>();
  for (auto const &pair : storage_map_) {
    keys.push_back(pair.first);
  }

  EXPECT_NE(keys[0], keys[1]);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
