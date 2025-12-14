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
constexpr const char *kHogdProtocol = protocol_commit;
constexpr const char *kHogdCmake = cmake_repro;
// NOLINTEND(readability-identifier-naming)

} // namespace hogd::version
