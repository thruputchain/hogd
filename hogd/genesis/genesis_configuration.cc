#include "hogd/genesis/genesis_configuration.h"
#include "hogd/genesis/genesis_constants.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "built_contracts/contract_loader.h"
#include "built_contracts/genesis_root.h"
#include "built_contracts/genesis_root.hh"
#include "built_contracts/initial_token_claim.h"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "precompiles.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {
std::tuple<std::uint64_t, std::uint64_t>
make_contract_u128(uint128_t value)
{
  return {static_cast<std::uint64_t>(value >> 64),
          static_cast<std::uint64_t>(value)};
}

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
                                    contract->wasmi_contract().data.size()),
                                .contract_type = ContractType::kWasmi};
  case ContractType::kRv32:
    return DeployedContractHash{.hash = sha256(*contract),
                                .contract_size = static_cast<uint32_t>(
                                    contract->rv32_contract().data.size()),
                                .contract_type = ContractType::kRv32};
  }
  // ContractType is an XDR union discriminant, rejected at parse time if it is
  // not one of the arms above, so this is unreachable.
  panic("make_deployed_contract_hash: unsupported contract type: {}",
        contract->contract_type());
}
} // namespace

DeployedContractHash
GenesisConfigurationLoader::native_default_wallet_hash()
{
  return make_deployed_contract_hash(make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet));
}

GenesisConfigurationLoader::GenesisConfigurationLoader(
    GenesisConfiguration genesis_configuration)
  : genesis_configuration_(std::move(genesis_configuration))
{}

bool
valid_genesis_configuration_v2(GenesisConfigurationV2 const &config)
{
  return config.root_contract.contract_type() == ContractType::kRv32 &&
         !config.root_contract.rv32_contract().data.empty();
}

void
visit_genesis_execution_prerequisites(
    GenesisConfigurationV2 const &config,
    std::function<void(std::shared_ptr<const Contract>)> const &visit_contract,
    std::function<void(ContractAddress const &,
                       DeployedContractHash const &)> const &visit_deployment,
    std::function<void(ContractAddress const &, AddressAuthData const &)> const
        &visit_auth_data)
{
  CHECK(valid_genesis_configuration_v2(config));

  auto root = std::make_shared<Contract>(config.root_contract);
  visit_contract(root);
  visit_deployment(kGenesisRootContractAddress,
                   make_deployed_contract_hash(root));

  AddressAuthData root_auth{};
  root_auth.required_weight = 0;
  root_auth.last_written_height = 0;
  visit_auth_data(kGenesisRootContractAddress, root_auth);

  auto token = make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractHogToken);
  visit_contract(token);
  visit_deployment(kNativeTokenContractAddress,
                   make_deployed_contract_hash(token));

  auto wallet = make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet);
  visit_contract(wallet);
}

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

  // The default wallet precompile: contract-store record only, no deployment
  // row. It is reachable solely via root-resolution fallback at addresses
  // with no deployed contract (SystemCallHandler::resolve_runtime).
  write_contract(make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet));
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
            .invocation =
                client.template make_invocation<u8"claim_initial_tokens">(
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

std::shared_ptr<const Contract>
load_stock_genesis_root_contract()
{
  auto contract = std::make_shared<Contract>(ContractType::kRv32);
  contract->rv32_contract() = load_contract_blob("genesis_root.rv32a");
  return contract;
}

GenesisConfigurationV2
GenesisConfigurationBuilder::build_v2(NetworkHash const &network_hash,
                                      StorageCostModel const &cost_model,
                                      Contract const &root_contract) const
{
  CHECK_EQ(root_contract.contract_type(), ContractType::kRv32);
  CHECK(!root_contract.rv32_contract().data.empty());
  return GenesisConfigurationV2{
      .network_hash = network_hash,
      .storage_cost_model = cost_model.to_xdr(),
      .root_contract = root_contract,
      .default_wallet = default_wallet_override_.value_or(
          GenesisConfigurationLoader::native_default_wallet_hash()),
  };
}

std::vector<SignedTransaction>
GenesisConfigurationBuilder::make_genesis_ceremony_transactions(
    StorageCostModel const &cost_model) const
{
  using root_client_t = ContractTraitClient<genesis_root::GenesisRoot>;
  root_client_t client(kGenesisRootContractAddress);
  constexpr static GasAmount kDistributionGas =
      std::numeric_limits<GasAmount>::max();

  // One transaction creates all economic state. It funds the first root
  // balance and replay-cache rows, plus the native-token balance and supply
  // rows, exactly once; no zero-valued economic rows are needed at height 0.
  const uint128_t root_funding =
      static_cast<uint128_t>(cost_model.get_balance_cost()) +
      static_cast<uint128_t>(cost_model.get_replay_cache_cost(1));
  const uint128_t token_funding =
      static_cast<uint128_t>(cost_model.get_balance_cost()) +
      static_cast<uint128_t>(cost_model.get_nnint_cost());

  x23::Vec<genesis_root::Distribution, UINT32_MAX> distributions;
  decltype(Transaction{}.contracts_to_deploy) contracts_to_deploy;
  distributions.reserve(entries_.size());
  contracts_to_deploy.reserve(entries_.size());
  for (auto const &entry : entries_) {
    CHECK_GE(entry.amount, 0);
    distributions.push_back(genesis_root::Distribution{
        .nonce = entry.config.initial_nonce,
        .contract_hash = entry.config.contract_hash,
        .initializer = entry.config.entrypoint,
        .initializer_calldata = entry.config.calldata,
        .allocation = to_xdr(static_cast<uint128_t>(entry.amount)),
    });
    contracts_to_deploy.push_back(*entry.deployed_contract);
  }

  return {SignedTransaction{
      .tx = Transaction{
          .invocation = client.make_invocation<u8"distribute">(
              kDistributionGas, distributions, make_contract_u128(root_funding),
              make_contract_u128(token_funding)),
          .uniquifier =
              {
                  .block_expiry_height =
                      std::numeric_limits<BlockHeight>::max(),
                  .nonce = 0,
              },
          .contracts_to_deploy = std::move(contracts_to_deploy),
      }}};
}

StorageKey
GenesisConfigurationLoader::calculate_storage_key(
    SHA256Hash const &hash, SHA256Hash const &nonce,
    ContractEntrypointID entrypoint, std::span<const std::uint8_t> calldata)
{
  // Create the key data: "Initial Deployment" || hash || nonce || entrypoint ||
  // calldata
  std::vector<std::uint8_t> key_data;

  // Add prefix. Use a `u8"..."` literal so the hashed bytes are the standard's
  // UTF-8 (== ASCII, here) encoding rather than implementation-defined
  // ordinary-literal code units; otherwise this storage key would depend on the
  // execution character set (`-fexec-charset`) and could diverge across honest
  // builds. Must stay byte-identical to the reader-side derivation in
  // precompile_initial_token_claim.cc. See hogd issue #2573.
  std::u8string_view prefix = u8"Initial Deployment";
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

std::optional<GenesisConfiguration>
GenesisConfigurationBuilder::build(StorageCostModel const &cost_model) const
{
  if (allocated_hogs_ >
      GenesisConfigurationLoader::initial_hog_token_supply()) {
    return std::nullopt;
  }

  GenesisConfiguration genesis_configuration;
  for (auto const &entry : entries_) {
    genesis_configuration.initial_config.push_back(InitialConfigurationEntry{
        .config = entry.config,
        .amount = to_xdr(entry.amount),
    });
  }

  genesis_configuration.storage_cost_model = cost_model.to_xdr();
  genesis_configuration.default_wallet = default_wallet_override_.value_or(
      GenesisConfigurationLoader::native_default_wallet_hash());
  return genesis_configuration;
}

GenesisConfiguration
GenesisConfigurationBuilder::build_or_panic(
    StorageCostModel const &cost_model) const
{
  std::optional<GenesisConfiguration> genesis_configuration = build(cost_model);
  if (!genesis_configuration) {
    panic("GenesisConfigurationBuilder: allocated hogs exceeds initial hog "
          "token supply");
  }
  return std::move(*genesis_configuration);
}
} // namespace hogd
