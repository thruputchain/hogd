#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/storage_cost/storage_cost_model.h"
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

class GenesisConfigurationBuilder {
public:
  void add_initial_config(std::shared_ptr<const Contract> const &contract,
                          SHA256Hash const &nonce,
                          ContractEntrypointID entrypoint, Calldata calldata,
                          int128_t amount);

  // Returns std::nullopt if the configured allocations exceed the initial hog
  // token supply. Callers build this from user-supplied data (devnode bootstrap
  // manifests), so an over-allocation is reportable input, not an invariant:
  // report it rather than taking the process down from inside a shared builder.
  std::optional<GenesisConfiguration>
  build(StorageCostModel const &cost_model) const;

  // build() for callers whose allocations are hardcoded and therefore cannot
  // exceed the supply -- tests, benchmarks, loadgen. Panics if they somehow do.
  // Anything assembling a config from user input must use build() and report
  // the failure instead.
  GenesisConfiguration build_or_panic(StorageCostModel const &cost_model) const;

  // The set of transactions that claim the balances that are set up
  // in the genesis block.
  // These could be, but are not required to be, all run in the first block.
  std::vector<SignedTransaction> make_unfurl_genesis_transactions() const;

  // Build the policy-neutral height-0 configuration. V2 contains no
  // allocations or native supply; those are created by root-contract
  // invocations in the height-1 ceremony.
  GenesisConfigurationV2 build_v2(NetworkHash const &network_hash,
                                  StorageCostModel const &cost_model,
                                  Contract const &root_contract) const;

  // Build the stock distributor root's single height-1 invocation. The
  // transaction creates and funds all economic state and deploys every entry.
  std::vector<SignedTransaction>
  make_genesis_ceremony_transactions(StorageCostModel const &cost_model) const;

  // Override GenesisConfiguration::default_wallet (defaults to
  // native_default_wallet_hash()).
  void set_default_wallet(DeployedContractHash const &default_wallet)
  {
    default_wallet_override_ = default_wallet;
  }

private:
  struct Entry {
    PresetDeploymentConfig config;
    int128_t amount;
    std::shared_ptr<const Contract> deployed_contract;
  };

  std::vector<Entry> entries_;
  int128_t allocated_hogs_ = 0;
  std::optional<DeployedContractHash> default_wallet_override_;
};

// Load the repository's reviewed stock distributor artifact. Chains may put a
// different RV32 contract in GenesisConfigurationV2::root_contract.
std::shared_ptr<const Contract> load_stock_genesis_root_contract();

class GenesisConfigurationLoader {
public:
  // Constructor takes the genesis configuration
  explicit GenesisConfigurationLoader(
      GenesisConfiguration genesis_configuration);

  // Load genesis configuration data into storage
  // This calculates storage keys and stores allocation amounts as int128s
  void load_genesis_configuration(
      // Returned amount is the amount of storage used by the item
      std::function<int128_t(StorageKey const &, int128_t const &)> const
          &write_storage,
      std::function<void(std::shared_ptr<const Contract>)> const
          &write_contract,
      std::function<void(ContractAddress const &contract_address,
                         DeployedContractHash const &contract_hash)> const
          &write_deployment,
      std::function<int128_t(ContractAddress const &,
                             AddressAuthData const &)> const &write_auth_data);

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

  // DeployedContractHash of the native default_wallet precompile's contract
  // record — the record load_genesis_configuration writes unconditionally.
  // Canonical default for GenesisConfiguration::default_wallet.
  static DeployedContractHash native_default_wallet_hash();

private:
  GenesisConfiguration genesis_configuration_;
};

[[nodiscard]] bool
valid_genesis_configuration_v2(GenesisConfigurationV2 const &config);

// Visit only the records required to execute the height-1 root ceremony.
// Economic state (supply, balances, and replay metadata) is created lazily by
// ordinary transaction execution.
void visit_genesis_execution_prerequisites(
    GenesisConfigurationV2 const &config,
    std::function<void(std::shared_ptr<const Contract>)> const &visit_contract,
    std::function<void(ContractAddress const &,
                       DeployedContractHash const &)> const &visit_deployment,
    std::function<void(ContractAddress const &, AddressAuthData const &)> const
        &visit_auth_data);

} // namespace hogd
