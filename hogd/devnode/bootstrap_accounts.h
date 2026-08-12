#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class GenesisConfigurationBuilder;

inline constexpr std::uint32_t kMaxBootstrapAccounts = 1000;
inline constexpr std::uint64_t kDefaultBootstrapAccountStorageTokens =
    10'000'000'000'000;

struct BootstrapAccountConfig {
  std::uint32_t count = 0;
  std::string seed = "groundhog-dev-accounts";
  std::uint64_t storage_tokens = kDefaultBootstrapAccountStorageTokens;
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
  GenesisConfiguration genesis_configuration;
  std::vector<SignedTransaction> initialization_txs;
  std::vector<BootstrapAccountMetadata> accounts;
};

std::vector<BootstrapAccountMetadata>
append_bootstrap_accounts(GenesisConfigurationBuilder &builder,
                          BootstrapAccountConfig const &config);

std::pair<Ed25519PublicKey, Ed25519SecretKey>
derive_bootstrap_account_keypair(std::string_view seed, std::uint32_t index);

BootstrapAccountArtifact
build_bootstrap_account_artifact(BootstrapAccountConfig const &config);

} // namespace hogd
