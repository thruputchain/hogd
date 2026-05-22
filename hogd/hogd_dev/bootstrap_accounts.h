#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "genesis.hh"
#include "hogd/base/types.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

inline constexpr std::uint32_t kMaxBootstrapAccounts = 1000;

struct BootstrapAccountConfig {
  std::uint32_t count = 0;
  std::string seed = "groundhog-dev-accounts";
  std::uint64_t storage_tokens = 1'000'000'000;
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

std::pair<Ed25519PublicKey, Ed25519SecretKey>
derive_bootstrap_account_keypair(std::string_view seed, std::uint32_t index);

BootstrapAccountArtifact
build_bootstrap_account_artifact(BootstrapAccountConfig const &config);

} // namespace hogd
