// version.h - version information

#pragma once

#include <format>
#include <string>

#include "hogd/base/log.h"

namespace hogd {
namespace version {

constexpr size_t kVersionMajor = 0;
constexpr size_t kVersionMinor = 0;
const std::string kVersion = std::format("{}.{}", kVersionMajor, kVersionMinor);

constexpr char kProtocol[] = "hogp0";

inline void
print_version()
{
  info("hogd version {}, protocol {}", kVersion, kProtocol);
}

} // namespace version
} // namespace hogd
