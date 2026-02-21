#pragma once

#include <cstdint>

namespace hogd {

/**
 * Table of gas costs for various operations.
 *
 * All of these values are placeholders.
 */
constexpr static uint64_t kSyscallGetBlockNumberCost = 1;
constexpr static uint64_t kSyscallGetPrevBlockHashCost = 10;

// NOLINTBEGIN(readability-identifier-naming)
constexpr static auto SHA256HashCost = [](uint64_t bytes_len) -> uint64_t {
  constexpr uint64_t kMaxSafeLen = (UINT64_MAX - 500) / 100;
  if (bytes_len > kMaxSafeLen) return UINT64_MAX;
  return (bytes_len * 100) + 500;
};
// NOLINTEND(readability-identifier-naming)

constexpr static uint64_t kNoSyscallCost = 10;
constexpr static uint64_t kGenericBadSyscallCost = 10;

constexpr static uint64_t kHasKeyCost = 100;

} // namespace hogd
