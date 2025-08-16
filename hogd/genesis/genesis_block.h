#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

constexpr static ContractAddress
make_precompile_address(uint64_t idx)
{
  ContractAddress addr{};
  addr[0] = (idx >> 56) & 0xFF;
  addr[1] = (idx >> 48) & 0xFF;
  addr[2] = (idx >> 40) & 0xFF;
  addr[3] = (idx >> 32) & 0xFF;
  addr[4] = (idx >> 24) & 0xFF;
  addr[5] = (idx >> 16) & 0xFF;
  addr[6] = (idx >> 8) & 0xFF;
  addr[7] = idx & 0xFF;
  return addr;
}

class GenesisBlockLoader {
public:
  // Constructor takes the genesis block
  explicit GenesisBlockLoader(GenesisBlock genesis_block);

  // Load genesis block data into storage
  // This calculates storage keys and stores allocation amounts as int128s
  void load_genesis_block(
      std::function<void(StorageKey const &, int128_t const&)> const
          &write_storage,
      std::function<void(std::shared_ptr<const Contract>)> const
          &write_contract,
      std::function<void(ContractAddress const &contract_address,
                         SHA256Hash const &contract_hash)> const
          &write_deployment);

  // Calculate storage key for a genesis entry
  // Format: sha256("Initial Deployment" || hash || nonce || entrypoint ||
  // calldata)
  static StorageKey
  calculate_storage_key(SHA256Hash const &hash, SHA256Hash const &nonce,
                        ContractEntrypointID entrypoint,
                        std::span<const std::uint8_t> calldata);

  constexpr static ContractAddress kDeployContractAddress =
      make_precompile_address(kDeployContractPrecompileIndex);
  constexpr static ContractAddress kPresetNativeTokenAddress =
      make_precompile_address(kPresetNativeTokenPrecompileIndex);

private:
  GenesisBlock genesis_block_;
};

} // namespace hogd
