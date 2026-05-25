#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "hogd/hogd_dev/bootstrap_accounts.h"

namespace hogd {

// Default human protocol label stamped into the runtime manifest's
// `networkId` field when no bootstrap manifest overrides it. The CLI's
// default and the node-construction call site reference this shared
// constant so they cannot drift apart.
inline constexpr std::string_view kDefaultRuntimeNetworkId = "groundhog-local";

struct BootstrapManifestConfig {
  std::string network_id;
  BootstrapAccountConfig bootstrap_config;
};

// Reads, parses, and validates a bootstrap manifest file.
// Throws std::runtime_error on any validation failure.
BootstrapManifestConfig
load_bootstrap_manifest(std::filesystem::path const &path);

} // namespace hogd
