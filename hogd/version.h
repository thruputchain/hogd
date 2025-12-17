// -*-C++-*-

#pragma once

extern "C" {
extern const char hogd_version[];
extern const char hogd_commit[];
extern const char protocol_commit[];
extern const char cmake_repro[];
}

namespace hogd::version {

// NOLINTBEGIN(readability-identifier-naming)
constexpr const char *kHogdVersion = hogd_version;
constexpr const char *kHogdGitHash = hogd_commit;
inline const char *
kHogdProtocol()
{
  static const std::string kProtocol =
      "hogp/" + std::string(protocol_commit, 12);
  return kProtocol.c_str();
}
constexpr const char *kHogdCmake = cmake_repro;
// NOLINTEND(readability-identifier-naming)

} // namespace hogd::version
