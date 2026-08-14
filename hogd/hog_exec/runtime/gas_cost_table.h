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

// Ed25519 signature verification performs fixed elliptic-curve work plus a
// SHA-512 pass over the message, so it is priced as a base charge plus a
// per-message-byte charge, mirroring the SHA-256 model. The base is larger than
// the SHA-256 base to reflect the two scalar multiplications the verifier does
// regardless of message length.
//
// This charge is levied at runtime by syscall_VerifySignature_inner (the value
// it returns is debited by the shared syscall gas epilogue for both the WASM
// and RV32 paths), so it is deterministic and consensus-safe across VMs. It is
// deliberately NOT added to kSyscallGasCostSchedule below: that array is folded
// into rv32::kGasScheduleDigest, and bumping the digest would reject every
// already-lowered RV32 artifact until a matching rv32-lower release re-stamped
// it. Folding this cost into the committed schedule is left for the next
// coordinated gas-schedule version bump. See hogd issue #3360.
constexpr static uint64_t kEd25519VerifyByteCost = 100;
constexpr static uint64_t kEd25519VerifyBaseCost = 2000;

// NOLINTBEGIN(readability-identifier-naming)
constexpr static auto SHA256HashCost = [](uint64_t bytes_len) -> uint64_t {
  constexpr uint64_t kMaxSafeLen =
      (UINT64_MAX - kSHA256HashBaseCost) / kSHA256HashByteCost;
  if (bytes_len > kMaxSafeLen)
    return UINT64_MAX;
  return (bytes_len * kSHA256HashByteCost) + kSHA256HashBaseCost;
};

constexpr static auto Ed25519VerifyCost = [](uint64_t bytes_len) -> uint64_t {
  constexpr uint64_t kMaxSafeLen =
      (UINT64_MAX - kEd25519VerifyBaseCost) / kEd25519VerifyByteCost;
  if (bytes_len > kMaxSafeLen)
    return UINT64_MAX;
  return (bytes_len * kEd25519VerifyByteCost) + kEd25519VerifyBaseCost;
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
