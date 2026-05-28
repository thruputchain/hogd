#pragma once

#include <array>
#include <cstdint>

namespace hogd {

/**
 * Table of gas costs for various operations.
 *
 * All of these values are placeholders.
 */
constexpr static uint64_t kSHA256HashByteCost = 100;
constexpr static uint64_t kSHA256HashBaseCost = 500;

// NOLINTBEGIN(readability-identifier-naming)
constexpr static auto SHA256HashCost = [](uint64_t bytes_len) -> uint64_t {
  constexpr uint64_t kMaxSafeLen =
      (UINT64_MAX - kSHA256HashBaseCost) / kSHA256HashByteCost;
  if (bytes_len > kMaxSafeLen)
    return UINT64_MAX;
  return (bytes_len * kSHA256HashByteCost) + kSHA256HashBaseCost;
};
// NOLINTEND(readability-identifier-naming)

constexpr static uint64_t kNoSyscallCost = 10;
constexpr static uint64_t kGenericBadSyscallCost = 10;

struct SyscallGasCostEntry {
  const char *name;
  uint64_t cost;
};

// Current nonzero direct host-side syscall charges. Most syscalls intentionally
// return zero direct host gas today; child invocation gas is metered by the
// invoked VM or WASM runtime rather than a fixed syscall-cost constant.
constexpr std::array<SyscallGasCostEntry, 4> kSyscallGasCostSchedule = {{
    {.name = "SHA256HashBase", .cost = kSHA256HashBaseCost},
    {.name = "SHA256HashByte", .cost = kSHA256HashByteCost},
    {.name = "NoSyscall", .cost = kNoSyscallCost},
    {.name = "GenericBadSyscall", .cost = kGenericBadSyscallCost},
}};

} // namespace hogd
