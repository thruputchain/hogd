#pragma once

#include <array>
#include <cstdint>

namespace hogd {

/**
 * Table of gas costs for various operations.
 *
 * These values are placeholders, but RV32 artifacts commit to them:
 * kSyscallGasCostSchedule is folded into rv32::kGasScheduleDigest and pinned in
 * artifact.h. Changing any cost/name/order is a coordinated gas-schedule
 * update: update the pinned digest/version deliberately and regenerate
 * committed RV32 artifacts/lowerer constants.
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

// Direct host-side gas for the DeployContract syscall's contract resolution:
// a HogDB read, XDR decode, eager RV32 translation, and block-cache admission
// (see StorageSnapshotCache::get_contract_entry). Charged deterministically
// from the resolved contract's serialized size so the amount is independent of
// per-node / per-block cache state (a cache hit and a miss cost the same). This
// meters host work that is otherwise unaccounted when a contract is deployed
// but never invoked. Unlike the SHA256 costs above, this charge is applied by
// the host at runtime and is NOT compiled into RV32 artifacts, so it is
// deliberately kept out of kSyscallGasCostSchedule / kGasScheduleDigest.
// (hogd issue #3122)
constexpr static uint64_t kDeployContractLoadBaseCost = 1000;
constexpr static uint64_t kDeployContractLoadByteCost = 100;

// NOLINTBEGIN(readability-identifier-naming)
constexpr static auto DeployContractLoadCost = [](uint64_t bytes_len) -> uint64_t {
  constexpr uint64_t kMaxSafeLen =
      (UINT64_MAX - kDeployContractLoadBaseCost) / kDeployContractLoadByteCost;
  if (bytes_len > kMaxSafeLen)
    return UINT64_MAX;
  return (bytes_len * kDeployContractLoadByteCost) + kDeployContractLoadBaseCost;
};
// NOLINTEND(readability-identifier-naming)

constexpr static uint64_t kNoSyscallCost = 10;
constexpr static uint64_t kGenericBadSyscallCost = 10;

struct SyscallGasCostEntry {
  // `u8"..."` literal: folded into the RV32 gas-schedule digest (an artifact
  // wire constant), so the hashed bytes must be the standard's UTF-8 code units
  // rather than implementation-defined ordinary-literal bytes that depend on
  // the execution character set (`-fexec-charset`). See hogd issue #2689.
  const char8_t *name;
  uint64_t cost;
};

// Current nonzero direct host-side syscall charges. Most syscalls intentionally
// return zero direct host gas today; child invocation gas is metered by the
// invoked VM or WASM runtime rather than a fixed syscall-cost constant.
constexpr std::array<SyscallGasCostEntry, 4> kSyscallGasCostSchedule = {{
    {.name = u8"SHA256HashBase", .cost = kSHA256HashBaseCost},
    {.name = u8"SHA256HashByte", .cost = kSHA256HashByteCost},
    {.name = u8"NoSyscall", .cost = kNoSyscallCost},
    {.name = u8"GenericBadSyscall", .cost = kGenericBadSyscallCost},
}};

} // namespace hogd
