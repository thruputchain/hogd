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

inline constexpr std::uint32_t kMaxDevAccounts = 1000;

struct DevAccountBootstrapConfig {
  std::uint32_t count = 0;
  std::string seed = "groundhog-dev-accounts";
  std::uint64_t storage_tokens = 1'000'000'000;
};

struct DevAccountMetadata {
  std::uint32_t index;
  ContractAddress address;
  Ed25519PublicKey public_key;
  std::uint64_t storage_tokens;
};

struct DevAccountBootstrapArtifact {
  GenesisConfiguration genesis_configuration;
  std::vector<SignedTransaction> initialization_txs;
  std::vector<DevAccountMetadata> accounts;
};

std::pair<Ed25519PublicKey, Ed25519SecretKey>
derive_dev_account_keypair(std::string_view seed, std::uint32_t index);

DevAccountBootstrapArtifact
build_dev_account_bootstrap_artifact(DevAccountBootstrapConfig const &config);

} // namespace hogd
