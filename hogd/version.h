// -*-C++-*-

#pragma once

#include <string>
#include <string_view>

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
  // The protocol token is compared byte-for-byte against peers at the mempool
  // intro RPC boundary, so its bytes must be charset-independent. Build the
  // "hogp/" prefix from a `u8"..."` literal (standard-mandated UTF-8 == ASCII
  // bytes) rather than an ordinary literal whose object bytes depend on
  // `-fexec-charset`; `protocol_commit` is likewise pinned to UTF-8 in
  // cmake/version.c.in. Otherwise two honest peers built with different
  // execution charsets would derive byte-unequal tokens and reject each other.
  // See hogd issue #2728.
  static const std::string kProtocol = [] {
    constexpr std::u8string_view kPrefix = u8"hogp/";
    std::string token(reinterpret_cast<char const *>(kPrefix.data()),
                      kPrefix.size());
    token.append(protocol_commit, 12);
    return token;
  }();
  return kProtocol.c_str();
}
constexpr const char *kHogdCmake = cmake_repro;
// NOLINTEND(readability-identifier-naming)

} // namespace hogd::version
