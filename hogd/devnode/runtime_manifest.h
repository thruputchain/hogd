#pragma once

#include "types.hh"
#include <filesystem>
#include <string>

namespace hogd {

class HogDevnodeNode;

struct RuntimeManifestPointer {
  std::filesystem::path manifest_path;
  SHA256Hash manifest_hash;
  std::string readiness_payload;
};

std::string format_hex_hash(SHA256Hash const &hash);

RuntimeManifestPointer
write_hog_devnode_runtime_manifest(HogDevnodeNode const &node,
                                   NetworkHash const &network_hash);

} // namespace hogd
