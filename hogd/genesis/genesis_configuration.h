#pragma once

#include <cstdint>
#include <expected>
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
#include "hogd/genesis/genesis_constants.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

enum class GenesisCeremonyError {
  kNegativeDistribution,
  kSupplyExceeded,
};

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
  void add_distribution(std::shared_ptr<const Contract> const &contract,
                        SHA256Hash const &nonce,
                        ContractEntrypointID entrypoint, Calldata calldata,
                        int128_t amount);

  // Build the policy-neutral height-0 configuration. It contains no
  // allocations or native supply; those are created by root-contract
  // invocations in the height-1 ceremony.
  GenesisConfiguration build(NetworkHash const &network_hash,
                             StorageCostModel const &cost_model,
                             Contract const &root_contract) const;

  // Build the stock distributor root's single height-1 invocation. The
  // transaction creates and funds all economic state and deploys every entry.
  std::expected<std::vector<SignedTransaction>, GenesisCeremonyError>
  try_make_genesis_ceremony_transactions(
      StorageCostModel const &cost_model) const;

  // Panic-style wrapper for trusted callers whose hardcoded allocations are
  // known to fit within the native-token supply.
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
    SHA256Hash initial_nonce;
    SHA256Hash contract_hash;
    ContractEntrypointID entrypoint;
    Calldata calldata;
    int128_t amount;
    std::shared_ptr<const Contract> deployed_contract;
  };

  std::vector<Entry> entries_;
  std::optional<DeployedContractHash> default_wallet_override_;
};

// Load the repository's reviewed stock distributor artifact. Chains may put a
// different RV32 contract in GenesisConfiguration::root_contract.
std::shared_ptr<const Contract> load_stock_genesis_root_contract();

ContractAddress
calculate_genesis_deployment_address(SHA256Hash const &contract_hash,
                                     SHA256Hash const &nonce);

DeployedContractHash native_default_wallet_hash();

[[nodiscard]] bool
valid_genesis_configuration(GenesisConfiguration const &config);

// Visit only the records required to execute the height-1 root ceremony.
// Economic state (supply, balances, and replay metadata) is created lazily by
// ordinary transaction execution.
void visit_genesis_execution_prerequisites(
    GenesisConfiguration const &config,
    std::function<void(std::shared_ptr<const Contract>)> const &visit_contract,
    std::function<void(ContractAddress const &,
                       DeployedContractHash const &)> const &visit_deployment,
    std::function<void(ContractAddress const &, AddressAuthData const &)> const
        &visit_auth_data);

} // namespace hogd
