#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "transaction.hh"

namespace hogd {

inline constexpr std::string_view kDefaultHostedDevnetNetworkId = "devnet";

struct HostedBootstrapGeneratorConfig {
  std::string network_id = std::string(kDefaultHostedDevnetNetworkId);
  BootstrapAccountConfig bootstrap_config{
      .count = 10,
      .seed = "groundhog-devnet",
      .storage_tokens = 1'000'000'000,
      .dev_wallet_profile = std::string(kDefaultDevWalletProfile),
  };
};

struct HostedBootstrapArtifact {
  GenesisConfiguration genesis_configuration;
  std::vector<SignedTransaction> initialization_txs;
  std::vector<BootstrapAccountMetadata> accounts;
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
