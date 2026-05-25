#pragma once

#include <filesystem>
#include <string>

#include "hogd/hogd_dev/bootstrap_accounts.h"

namespace hogd {

struct BootstrapManifestConfig {
  std::string network_id;
  BootstrapAccountConfig bootstrap_config;
};

// Reads, parses, and validates a bootstrap manifest file.
// Throws std::runtime_error on any validation failure.
BootstrapManifestConfig
load_bootstrap_manifest(std::filesystem::path const &path);

} // namespace hogd
