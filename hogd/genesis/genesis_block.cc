#include "hogd/genesis/genesis_block.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

GenesisBlockLoader::GenesisBlockLoader(GenesisBlock genesis_block)
  : genesis_block_(std::move(genesis_block))
{}

void
GenesisBlockLoader::load_genesis_block(
    std::function<void(StorageKey const &, Bytestring const &)> const
        &write_storage,
    std::function<void(std::shared_ptr<const Contract> contract)> const
        &write_contract,
    std::function<void(ContractAddress const &contract_address,
                       SHA256Hash const &contract_hash)> const
        &write_deployment)
{
  // Process each initial configuration entry
  for (auto const &entry : genesis_block_.initial_config) {
    // Calculate storage key based on the preset contract configuration
    auto const &config = entry.config;
    auto storage_key = calculate_storage_key(
        config.contract_hash, config.initial_nonce,
        ContractEntrypointID{}, // Assuming initialize is entrypoint 0
        config.initialize_calldata);

    // Convert int128_xdr to int128_t and create allocation bytestring
    auto amount = from_xdr(entry.amount);
    auto allocation_bytestring = create_allocation_bytestring(amount);

    // Write to storage
    write_storage(storage_key, allocation_bytestring);
  }

  auto deploy_contract = load_contract("initial_token_claim.wasm");
  auto contract_hash = sha256(*deploy_contract);

  write_contract(deploy_contract);
  write_deployment(kDeployContractAddress, contract_hash);
}

StorageKey
GenesisBlockLoader::calculate_storage_key(
    SHA256Hash const &hash, SHA256Hash const &nonce,
    ContractEntrypointID entrypoint, std::span<const std::uint8_t> calldata)
{
  // Create the key data: "Initial Deployment" || hash || nonce || entrypoint ||
  // calldata
  std::vector<std::uint8_t> key_data;

  // Add prefix
  std::string_view prefix = "Initial Deployment";
  key_data.insert(key_data.end(), prefix.begin(), prefix.end());

  // Add hash
  key_data.insert(key_data.end(), hash.begin(), hash.end());

  // Add nonce
  key_data.insert(key_data.end(), nonce.begin(), nonce.end());

  // Add entrypoint (as big-endian bytes)
  key_data.insert(key_data.end(), entrypoint.begin(), entrypoint.end());

  // Add calldata
  key_data.insert(key_data.end(), calldata.begin(), calldata.end());

  // Calculate SHA256 hash
  auto hash_result = sha256(key_data);

  // Create storage key
  StorageKey storage_key(StorageKeyType::kStorageKeyTypeContractData);
  std::copy_n(hash_result.begin(), 32, storage_key.contract_data_key().begin());

  return storage_key;
}

Bytestring
GenesisBlockLoader::create_allocation_bytestring(int128_t amount)
{
  // Convert amount to big-endian bytes
  std::array<std::uint8_t, 16> amount_bytes;
  std::memcpy(amount_bytes.data(), &amount, 16);

  // Create bytestring
  Bytestring bytestring;
  bytestring.resize(16);
  std::copy_n(amount_bytes.begin(), 16, bytestring.begin());

  return bytestring;
}

} // namespace hogd
