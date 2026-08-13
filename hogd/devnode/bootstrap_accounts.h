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
    1'000'000'000;
inline constexpr std::uint64_t kLocalFaucetStorageTokens = 10'000'000'000'000;
inline constexpr std::uint64_t kDefaultLocalUserStorageTokens = 1'000'000'000;

struct BootstrapAccountConfig {
  std::uint32_t count = 0;
  std::string seed = "groundhog-dev-accounts";
  std::uint64_t storage_tokens = kDefaultBootstrapAccountStorageTokens;
  std::string dev_wallet_profile = std::string(kDefaultDevWalletProfile);
};

struct LocalBootstrapConfig {
  std::uint32_t user_account_count = 0;
  std::string seed = "groundhog-dev-accounts";
  std::uint64_t user_storage_tokens = kDefaultLocalUserStorageTokens;
  std::string dev_wallet_profile = std::string(kDefaultDevWalletProfile);
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

BootstrapAccountArtifact
build_bootstrap_account_artifact(BootstrapAccountConfig const &config);

BootstrapAccountArtifact
build_local_bootstrap_account_artifact(LocalBootstrapConfig const &config);

} // namespace hogd
