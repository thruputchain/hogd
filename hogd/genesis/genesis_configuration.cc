#include "hogd/genesis/genesis_configuration.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "built_contracts/initial_token_claim.h"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "precompiles.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

GenesisConfigurationLoader::GenesisConfigurationLoader(
    GenesisConfiguration genesis_configuration)
  : genesis_configuration_(std::move(genesis_configuration))
{}

namespace {
std::shared_ptr<const Contract>
make_native_contract(HogexecPrecompiledNativeContracts contract_id)
{
  auto out = std::make_shared<Contract>();
  out->contract_type(ContractType::kNative);
  out->native_contract_id() = contract_id;
  return out;
}

DeployedContractHash
make_deployed_contract_hash(std::shared_ptr<const Contract> const &contract)
{
  assert(contract);
  switch (contract->contract_type()) {
  case ContractType::kNative:
  case ContractType::kMockTest:
    return DeployedContractHash{.hash = sha256(*contract),
                                .contract_size = 0,
                                .contract_type = contract->contract_type()};
  case ContractType::kWasmi:
    return DeployedContractHash{.hash = sha256(*contract),
                                .contract_size = static_cast<uint32_t>(
                                    contract->wasmi_contract().wasm.size()),
                                .contract_type = ContractType::kWasmi};
  default:
    throw std::runtime_error(
        "make_deployed_contract_hash: unsupported contract type");
  }
}
} // namespace

int128_t
GenesisConfigurationLoader::initial_hog_token_supply()
{
  return int128_t{kTotalHogSupply} * int128_t{kHogDivisibility};
}

void
GenesisConfigurationLoader::load_genesis_configuration(
    std::function<int128_t(StorageKey const &, int128_t const &)> const
        &write_storage,
    std::function<void(std::shared_ptr<const Contract> contract)> const
        &write_contract,
    std::function<void(ContractAddress const &contract_address,
                       DeployedContractHash const &contract_hash)> const
        &write_deployment,
    std::function<int128_t(ContractAddress const &,
                           AddressAuthData const &)> const &write_auth_data)
{
  auto deploy_contract = make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractInitialTokenClaim);
  write_contract(deploy_contract);
  write_deployment(kDeployContractAddress,
                   make_deployed_contract_hash(deploy_contract));

  // Write the initial token allocation
  StorageKey allocation_key(StorageKeyType::FreeNativeTokenBalance);
  write_storage(allocation_key, initial_hog_token_supply());

  int128_t total_storage_costs = 0;

  AddressAuthData deployer_auth_data{};
  deployer_auth_data.required_weight = 0;
  deployer_auth_data.last_written_height = 0;

  total_storage_costs +=
      write_auth_data(kDeployContractAddress, deployer_auth_data);

  // Process each initial configuration entry
  for (auto const &entry : genesis_configuration_.initial_config) {
    // Calculate storage key based on the preset contract configuration
    auto storage_key = calculate_storage_key(entry.config);

    // Convert int128_xdr to int128_t and create allocation bytestring
    auto amount = from_xdr(entry.amount);

    // Write to storage
    total_storage_costs += write_storage(storage_key, amount);
  }

  // Overwrite the allocation with the updated amount
  write_storage(allocation_key,
                initial_hog_token_supply() - total_storage_costs);

  // write the precompiled token contract
  auto precompiled_token_contract = make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractHogToken);
  write_contract(precompiled_token_contract);
  write_deployment(kPresetNativeTokenAddress,
                   make_deployed_contract_hash(precompiled_token_contract));
}

std::vector<SignedTransaction>
GenesisConfigurationBuilder::make_unfurl_genesis_transactions() const
{
  std::vector<SignedTransaction> unfurl_transactions;
  using token_claim_client_t =
      ContractTraitClient<initial_token_claim::InitialTokenClaim>;
  token_claim_client_t client(
      GenesisConfigurationLoader::kDeployContractAddress);
  constexpr static GasAmount kClaimGas = 1'000'000'000;

  // This nonce is not checked or read during the unfurl transaction.
  uint64_t uniquifier_nonce = 0;
  for (auto const &entry : entries_) {
    auto const &config = entry.config;

    SignedTransaction tx{
        .tx = Transaction{
            .invocation = client.make_invocation<"claim_initial_tokens">(
                kClaimGas, config.contract_hash, config.initial_nonce,
                config.entrypoint, config.calldata),
            .uniquifier = {.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = uniquifier_nonce++},
            .contracts_to_deploy = {*entry.deployed_contract},

        }};
    unfurl_transactions.push_back(tx);
  }

  return unfurl_transactions;
}

StorageKey
GenesisConfigurationLoader::calculate_storage_key(
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
  StorageKey storage_key(StorageKeyType::ContractData);
  std::copy_n(hash_result.begin(), 32, storage_key.contract_data_key().begin());

  return storage_key;
}

StorageKey
GenesisConfigurationLoader::calculate_storage_key(
    PresetDeploymentConfig const &config)
{
  return calculate_storage_key(config.contract_hash, config.initial_nonce,
                               config.entrypoint, config.calldata);
}

ContractAddress
GenesisConfigurationLoader::calculate_deployed_address(
    SHA256Hash const &contract_hash, SHA256Hash const &nonce)
{
  ContractDeploymentHashPreimage preimage{
      .deployer_address = kDeployContractAddress,
      .nonce = nonce,
      .contract_hash = contract_hash,
  };

  return sha256(preimage);
}

void
GenesisConfigurationBuilder::add_initial_config(
    std::shared_ptr<const Contract> const &contract, SHA256Hash const &nonce,
    ContractEntrypointID entrypoint, Calldata calldata, int128_t amount)
{
  entries_.push_back({
      .config =
          PresetDeploymentConfig{
              .initial_nonce = nonce,
              .contract_hash = sha256(*contract),
              .entrypoint = entrypoint,
              .calldata = std::move(calldata),
          },
      .amount = amount,
      .deployed_contract = contract,
  });
  allocated_hogs_ += amount;
}

GenesisConfiguration
GenesisConfigurationBuilder::build(StorageCostModel const &cost_model) const
{
  GenesisConfiguration genesis_configuration;
  for (auto const &entry : entries_) {
    genesis_configuration.initial_config.push_back(InitialConfigurationEntry{
        .config = entry.config,
        .amount = to_xdr(entry.amount),
    });
  }

  genesis_configuration.storage_cost_model = cost_model.to_xdr();

  if (allocated_hogs_ >
      GenesisConfigurationLoader::initial_hog_token_supply()) {
    throw std::runtime_error(
        "GenesisConfigurationBuilder::build: allocated hogs "
        "exceeds initial hog token supply");
  }
  return genesis_configuration;
}
} // namespace hogd
