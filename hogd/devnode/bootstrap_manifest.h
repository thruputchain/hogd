// -*-C++-*-

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "hogd/devnode/bootstrap_accounts.h"
#include "types.hh"

namespace hogd {

// Network hash stamped into the runtime manifest when no bootstrap manifest
// overrides it.
inline constexpr NetworkHash kDefaultRuntimeNetworkHash{};

struct BootstrapManifestConfig {
  NetworkHash network_hash;
  LocalBootstrapConfig bootstrap_config;
};

// Reads, parses, and validates a bootstrap manifest file. On any failure --
// I/O, JSON syntax, or schema validation -- returns the human-readable
// diagnostic instead of the config. The message is for display only: callers
// print it and exit; nothing branches on its text.
std::expected<BootstrapManifestConfig, std::string>
load_bootstrap_manifest(std::filesystem::path const &path);

} // namespace hogd
