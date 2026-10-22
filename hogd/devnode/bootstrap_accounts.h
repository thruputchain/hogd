#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class GenesisConfigurationBuilder;

inline constexpr std::uint32_t kMaxBootstrapAccounts = 1000;
inline constexpr std::uint64_t kDefaultBootstrapAccountStorageTokens =
    1'000'000'000;
inline constexpr std::uint64_t kLocalFaucetStorageTokens = 10'000'000'000'000;
inline constexpr std::uint64_t kDefaultLocalUserStorageTokens = 1'000'000'000;

struct BootstrapAccountConfig {
  std::uint32_t count = 0;
  std::string seed = "groundhog-dev-accounts";
  std::uint64_t storage_tokens = kDefaultBootstrapAccountStorageTokens;
};

using LocalNetworkConfigOverrides = std::map<std::string, std::uint64_t>;

// Parse a named decimal override that fits the parameter's type (uint32 for
// most, uint64 for max_tx_gas). Reject unknown names and duplicates.
void add_local_network_config_override(LocalNetworkConfigOverrides &overrides,
                                       std::string_view assignment);

// Apply overrides through the canonical config builder; database genesis
// validation remains authoritative for build-specific ceilings.
NetworkConfig
local_network_config(LocalNetworkConfigOverrides const &overrides);

// Public intent for a nonce-zero native account. Signers belong to callers.
struct LocalAccountAllocation {
  Ed25519PublicKey public_key{};
  std::uint64_t storage_tokens = kDefaultLocalUserStorageTokens;
  bool operator==(LocalAccountAllocation const &) const = default;
};

struct LocalBootstrapConfig {
  Ed25519PublicKey faucet_public_key{};
  std::vector<LocalAccountAllocation> accounts;
  LocalNetworkConfigOverrides network_config_overrides;
  bool operator==(LocalBootstrapConfig const &) const = default;
};

enum class BootstrapAccountRole {
  kFaucet,
  kLocalUser,
  kHostedDevAccount,
};

struct BootstrapAccountMetadata {
  std::uint32_t index;
  BootstrapAccountRole role;
  ContractAddress address;
  Ed25519PublicKey public_key;
  std::uint64_t initial_storage_tokens;
  std::uint64_t storage_tokens;
};

struct BootstrapAccountArtifact {
  GenesisConfiguration genesis_configuration;
  std::vector<SignedTransaction> initialization_txs;
  std::vector<BootstrapAccountMetadata> accounts;
};

std::vector<BootstrapAccountMetadata> append_bootstrap_accounts(
    GenesisConfigurationBuilder &builder, BootstrapAccountConfig const &config,
    BootstrapAccountRole role = BootstrapAccountRole::kHostedDevAccount);

std::pair<Ed25519PublicKey, Ed25519SecretKey>
derive_bootstrap_account_keypair(std::string_view seed, std::uint32_t index);

// Native pubkey account address for this signer, with account nonce zero.
ContractAddress
derive_bootstrap_account_address(Ed25519PublicKey const &public_key);

BootstrapAccountArtifact
build_bootstrap_account_artifact(BootstrapAccountConfig const &config,
                                 NetworkHash const &network_hash = {},
                                 StorageCostModel const &cost_model =
                                     StorageCostModel::default_cost_model());

BootstrapAccountArtifact build_local_bootstrap_account_artifact(
    LocalBootstrapConfig const &config, NetworkHash const &network_hash = {},
    StorageCostModel const &cost_model =
        StorageCostModel::default_cost_model());

} // namespace hogd
