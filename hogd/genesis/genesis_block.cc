#include "hogd/genesis/genesis_block.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "built_contracts/initial_token_claim.h"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "precompiles.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

GenesisBlockLoader::GenesisBlockLoader(GenesisBlock genesis_block)
  : genesis_block_(std::move(genesis_block))
{}

namespace {
std::shared_ptr<const Contract>
make_native_contract(HogexecPrecompiledNativeContracts contract_id)
{
  auto out = std::make_shared<Contract>();
  out->contract_type(ContractType::kNative);
  out->native_contract_id() = std::to_underlying(contract_id);
  return out;
}
} // namespace

void
GenesisBlockLoader::load_genesis_block(
    std::function<void(StorageKey const &, int128_t const &)> const
        &write_storage,
    std::function<void(std::shared_ptr<const Contract> contract)> const
        &write_contract,
    std::function<void(ContractAddress const &contract_address,
                       SHA256Hash const &contract_hash)> const
        &write_deployment)
{
  auto deploy_contract = load_contract("initial_token_claim.wasm");
  write_contract(deploy_contract);
  write_deployment(kDeployContractAddress, sha256(*deploy_contract));

  // write the initial token allocation
  StorageKey allocation_key(
      StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance);
  constexpr static int128_t kHogTokenTotalSupply =
      int128_t{kTotalHogSupply} * int128_t{kHogDivisibility};
  write_storage(allocation_key, kHogTokenTotalSupply);

  // Process each initial configuration entry
  for (auto const &entry : genesis_block_.initial_config) {
    // Calculate storage key based on the preset contract configuration
    auto storage_key = calculate_storage_key(entry.config);

    // Convert int128_xdr to int128_t and create allocation bytestring
    auto amount = from_xdr(entry.amount);

    // Write to storage
    write_storage(storage_key, amount);
  }

  // write the precompiled token contract
  auto precompiled_token_contract = make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractHogToken);
  write_contract(precompiled_token_contract);
  write_deployment(kPresetNativeTokenAddress,
                   sha256(*precompiled_token_contract));
}

std::vector<SignedTransaction>
GenesisBlockBuilder::make_unfurl_genesis_transactions()
{
  std::vector<SignedTransaction> unfurl_transactions;
  using token_claim_client_t =
      ContractTraitClient<initial_token_claim::InitialTokenClaim>;
  token_claim_client_t client(GenesisBlockLoader::kDeployContractAddress);
  constexpr static GasAmount kClaimGas = 1'000'000'000;

  for (auto const &entry : entries_) {
    auto const &config = entry.config;

    SignedTransaction tx{
        .tx = Transaction{
            .invocation = client.make_invocation<"claim_initial_tokens">(
                kClaimGas, config.contract_hash, config.initial_nonce,
                config.entrypoint, config.calldata),
            .contracts_to_deploy = {*entry.deployed_contract}}};
    unfurl_transactions.push_back(tx);
  }

  return unfurl_transactions;
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

StorageKey
GenesisBlockLoader::calculate_storage_key(PresetDeploymentConfig const &config)
{
  return calculate_storage_key(config.contract_hash, config.initial_nonce,
                               config.entrypoint, config.calldata);
}

ContractAddress
GenesisBlockLoader::calculate_deployed_address(SHA256Hash const& contract_hash, SHA256Hash const& nonce)
{
  ContractDeploymentHashPreimage preimage {
    .deployer_address = kDeployContractAddress,
    .nonce = nonce,
    .contract_hash = contract_hash,
  };

  return sha256(preimage);
}

void GenesisBlockBuilder::add_initial_config(std::shared_ptr<const Contract> const& contract, SHA256Hash const& nonce, ContractEntrypointID entrypoint, Calldata calldata, int128_t amount)
{
  entries_.push_back({
    .config = PresetDeploymentConfig{
      .initial_nonce = nonce,
      .contract_hash = sha256(*contract),
      .entrypoint = entrypoint,
      .calldata = std::move(calldata),
    },
    .amount = amount,
    .deployed_contract = contract,
  });
}

GenesisBlock GenesisBlockBuilder::build()
{
  GenesisBlock block;
  for (auto const& entry : entries_) {
    block.initial_config.push_back(InitialConfigurationEntry{
      .config = entry.config,
      .amount = to_xdr(entry.amount),
    });
  }
  return block;
}
} // namespace hogd
