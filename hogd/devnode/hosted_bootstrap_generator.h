#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

inline constexpr NetworkHash kDefaultHostedDevnetNetworkHash{};

struct HostedFaucetConfig {
  Ed25519PublicKey public_key;
  std::uint64_t storage_tokens = 0;
};

struct HostedFaucetMetadata {
  ContractAddress wallet_address;
  Ed25519PublicKey public_key;
  std::uint64_t initial_storage_tokens;
};

struct HostedBootstrapGeneratorConfig {
  NetworkHash network_hash = kDefaultHostedDevnetNetworkHash;
  BootstrapAccountConfig bootstrap_config{
      .count = 10,
      .seed = "groundhog-dev-accounts",
      .storage_tokens = 1'000'000'000,
      .dev_wallet_profile = std::string(kDefaultDevWalletProfile),
  };
  std::optional<HostedFaucetConfig> faucet;
};

struct HostedBootstrapArtifact {
  GenesisConfiguration genesis_configuration;
  std::vector<SignedTransaction> initialization_txs;
  std::vector<BootstrapAccountMetadata> accounts;
  std::optional<HostedFaucetMetadata> faucet;
  SHA256Hash genesis_hash;
  std::string genesis_json;
  std::string metadata_json;
};

HostedBootstrapArtifact
build_hosted_bootstrap_artifact(HostedBootstrapGeneratorConfig const &config);

void
write_hosted_bootstrap_artifact(HostedBootstrapGeneratorConfig const &config,
                                std::filesystem::path const &genesis_out,
                                std::filesystem::path const &metadata_out);

} // namespace hogd
