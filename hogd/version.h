// -*-C++-*-

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

/// Width of the protocol token, matching `typedef opaque ProtocolCommit[20]`
/// in protocol/network.x. This header stays free of the generated XDR
/// headers, so hog_db/rpc_from_gateway.cc carries the static_assert tying the
/// two together.
inline constexpr std::size_t kHogdProtocolLen = 20;

namespace detail {

/// The protocol token, built once and kept for the life of the process.
///
/// Peers compare these bytes for equality at the intro RPC boundary, so they
/// must be charset-independent. The "hogp/" prefix comes from a `u8"..."`
/// literal (standard-mandated UTF-8 == ASCII bytes) rather than an ordinary
/// literal whose object bytes depend on `-fexec-charset`; `protocol_commit`
/// is likewise pinned to UTF-8 in cmake/version.c.in. Otherwise two honest
/// peers built with different execution charsets would derive byte-unequal
/// tokens and reject each other. See hogd issue #2728.
///
/// `protocol_commit` is normally a 40-character commit id, but degrades to
/// "unknown" or "git-not-found" when the build cannot reach git. Copy at most
/// what is there and pad, so a short value cannot read past the end of the
/// literal, and so the token always fills the fixed-width array.
inline const std::array<std::uint8_t, kHogdProtocolLen> &
protocol_token()
{
  static const std::array<std::uint8_t, kHogdProtocolLen> kToken = [] {
    constexpr std::u8string_view kPrefix = u8"hogp/";
    static_assert(kPrefix.size() < kHogdProtocolLen);

    std::array<std::uint8_t, kHogdProtocolLen> token;
    token.fill('_');
    std::copy_n(kPrefix.begin(), kPrefix.size(), token.begin());

    const std::string_view commit{protocol_commit};
    const std::size_t width =
        std::min(commit.size(), kHogdProtocolLen - kPrefix.size());
    std::copy_n(commit.begin(), width, token.begin() + kPrefix.size());
    return token;
  }();
  return kToken;
}

/// The same bytes as a NUL-terminated C string, so the text accessor can hand
/// out a `const char *` exactly as it always has.
inline const char *
protocol_cstr()
{
  static const std::string kText = [] {
    const std::array<std::uint8_t, kHogdProtocolLen> &token = protocol_token();
    return std::string(reinterpret_cast<const char *>(token.data()),
                       token.size());
  }();
  return kText.c_str();
}

} // namespace detail

/// The protocol token as printable text, for runtime manifests, log lines,
/// and indexer outbox filenames. NUL-terminated and valid for the life of the
/// process.
inline const char *
kHogdProtocol()
{
  return detail::protocol_cstr();
}

/// The same token in the fixed-width form the wire carries. `ProtocolCommit`
/// is an array, so this always occupies exactly [kHogdProtocolLen] bytes and
/// costs no allocation to send or compare.
inline std::array<std::uint8_t, kHogdProtocolLen>
kHogdProtocolCommit()
{
  return detail::protocol_token();
}

constexpr const char *kHogdCmake = cmake_repro;
// NOLINTEND(readability-identifier-naming)

} // namespace hogd::version
