#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "account.hh"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

constexpr static ContractAddress
make_precompile_address(GenesisContractAddresses addr_enum)
{
  ContractAddress addr{};
  uint32_t underlying = std::to_underlying(addr_enum);
  addr[28] = (underlying >> 24) & 0xFF;
  addr[29] = (underlying >> 16) & 0xFF;
  addr[30] = (underlying >> 8) & 0xFF;
  addr[31] = underlying & 0xFF;
  static_assert(sizeof(addr) == 32);
  return addr;
}

class GenesisBlockBuilder {
public:
  void add_initial_config(std::shared_ptr<const Contract> const &contract,
                          SHA256Hash const &nonce,
                          ContractEntrypointID entrypoint, Calldata calldata,
                          int128_t amount);

  GenesisBlock build() const;

  // The set of transactions that claim the balances that are set up
  // in the genesis block.
  // These could be, but are not required to be, all run in the first block.
  std::vector<SignedTransaction> make_unfurl_genesis_transactions() const;

private:
  struct Entry {
    PresetDeploymentConfig config;
    int128_t amount;
    std::shared_ptr<const Contract> deployed_contract;
  };

  std::vector<Entry> entries_;
};

class GenesisBlockLoader {
public:
  // Constructor takes the genesis block
  explicit GenesisBlockLoader(GenesisBlock genesis_block);

  // Load genesis block data into storage
  // This calculates storage keys and stores allocation amounts as int128s
  void load_genesis_block(
      // Returned amount is the amount of storage used by the item
      std::function<int128_t(StorageKey const &, int128_t const &)> const
          &write_storage,
      std::function<void(std::shared_ptr<const Contract>)> const
          &write_contract,
      std::function<void(ContractAddress const &contract_address,
                         DeployedContractHash const &contract_hash)> const
          &write_deployment);

  // Calculate storage key for a genesis entry
  // Format: sha256("Initial Deployment" || hash || nonce || entrypoint ||
  // calldata)
  static StorageKey
  calculate_storage_key(SHA256Hash const &hash, SHA256Hash const &nonce,
                        ContractEntrypointID entrypoint,
                        std::span<const std::uint8_t> calldata);
  static StorageKey calculate_storage_key(PresetDeploymentConfig const &config);

  constexpr static ContractAddress kDeployContractAddress =
      make_precompile_address(
          GenesisContractAddresses::kAddressInitialTokenClaim);
  constexpr static ContractAddress kPresetNativeTokenAddress =
      make_precompile_address(
          GenesisContractAddresses::kAddressPrecompiledHogToken);

  static ContractAddress
  calculate_deployed_address(SHA256Hash const &contract_hash,
                             SHA256Hash const &nonce);

  static int128_t initial_hog_token_supply();

private:
  GenesisBlock genesis_block_;
};

} // namespace hogd
