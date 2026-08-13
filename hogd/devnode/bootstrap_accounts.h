#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class GenesisConfigurationBuilder;

inline constexpr std::uint32_t kMaxBootstrapAccounts = 1000;

struct BootstrapAccountConfig {
  std::uint32_t count = 0;
  std::string seed = "groundhog-dev-accounts";
  std::uint64_t storage_tokens = 1'000'000'000;
  std::string dev_wallet_profile = std::string(kDefaultDevWalletProfile);
};

struct BootstrapAccountMetadata {
  std::uint32_t index;
  ContractAddress address;
  Ed25519PublicKey public_key;
  std::uint64_t initial_storage_tokens;
  std::uint64_t storage_tokens;
};

struct BootstrapAccountArtifact {
  // Retained while consumers transition their persisted metadata from the V1
  // JSON shape. New chains must load genesis_configuration_v2 and execute
  // initialization_txs through the controlled height-1 ceremony.
  GenesisConfiguration genesis_configuration;
  GenesisConfigurationV2 genesis_configuration_v2;
  std::vector<SignedTransaction> initialization_txs;
  std::vector<BootstrapAccountMetadata> accounts;
};

std::vector<BootstrapAccountMetadata>
append_bootstrap_accounts(GenesisConfigurationBuilder &builder,
                          BootstrapAccountConfig const &config);

std::pair<Ed25519PublicKey, Ed25519SecretKey>
derive_bootstrap_account_keypair(std::string_view seed, std::uint32_t index);

BootstrapAccountArtifact
build_bootstrap_account_artifact(BootstrapAccountConfig const &config,
                                 NetworkHash const &network_hash = {},
                                 StorageCostModel const &cost_model =
                                     StorageCostModel::default_cost_model());

} // namespace hogd
