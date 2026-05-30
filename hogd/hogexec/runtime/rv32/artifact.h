#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/rv32/dispatch.h"
#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

inline constexpr std::array<uint8_t, 8> kArtifactMagic = {'G', 'H', 'R', 'V',
                                                          '3', '2', 'A', '1'};
inline constexpr uint32_t kArtifactFormatVersion = 3;
inline constexpr uint32_t kArtifactHeaderBytes = 172;
inline constexpr uint64_t kArtifactVmProfileDigest = 0x55c53a685f0eb33dULL;
inline constexpr uint32_t kArtifactEntryAbiVersion = 1;
// v1: register-native syscall ABI (args in a0..a7, no memory frame/footer).
inline constexpr uint32_t kArtifactSyscallAbiVersion = 1;
// Version of the RV32 gas schedule (instruction + host-syscall costs) stamped
// into the header at kArtifactGasVersionOffset and validated at load.
inline constexpr uint32_t kGasTableVersion = 2;
inline constexpr size_t kArtifactDigestBytes = 32;
inline constexpr size_t kArtifactFormatVersionOffset = 8;
inline constexpr size_t kArtifactHeaderSizeOffset = 12;
inline constexpr size_t kArtifactVmProfileOffset = 16;
inline constexpr size_t kArtifactFlagsOffset = 24;
inline constexpr size_t kArtifactEntryAbiOffset = 28;
inline constexpr size_t kArtifactSyscallAbiOffset = 32;
inline constexpr size_t kArtifactGasVersionOffset = 36;
inline constexpr size_t kArtifactGasDigestOffset = 40;
inline constexpr size_t kArtifactMemoryBytesOffset = 48;
inline constexpr size_t kArtifactCodeSizeOffset = 52;
inline constexpr size_t kArtifactInitialMemorySizeOffset = 56;
inline constexpr size_t kArtifactCodeOffsetOffset = 60;
inline constexpr size_t kArtifactInitialMemoryOffsetOffset = 64;
inline constexpr size_t kArtifactDispatchOffsetOffset = 68;
inline constexpr size_t kArtifactDispatchSizeOffset = 72;
inline constexpr size_t kArtifactCodeDigestOffset = 76;
inline constexpr size_t kArtifactInitialMemoryDigestOffset =
    kArtifactCodeDigestOffset + kArtifactDigestBytes;
inline constexpr size_t kArtifactDispatchDigestOffset =
    kArtifactInitialMemoryDigestOffset + kArtifactDigestBytes;

static_assert(kArtifactDispatchDigestOffset + kArtifactDigestBytes ==
              kArtifactHeaderBytes);

// FNV-1a digest over the full RV32 gas schedule: the instruction-cost table
// (from program.h) folded with the host-syscall cost schedule (from
// gas_cost_table.h). It is stamped into the artifact at
// kArtifactGasDigestOffset and re-derived here at load time, so an artifact
// built against a different schedule is rejected. This is an artifact/consensus
// concern, not an ISA-execution one, so it lives here rather than in the
// portable program.h core (which only needs the instruction costs themselves).
constexpr uint64_t
gas_schedule_digest_byte(uint64_t digest, uint8_t value)
{
  return fnv_digest_byte(digest, value);
}

constexpr uint64_t
gas_schedule_digest_u64(uint64_t digest, uint64_t value)
{
  return fnv_digest_u64(digest, value);
}

constexpr uint64_t
gas_schedule_digest_name(uint64_t digest, const char *name)
{
  for (const char *cursor = name; *cursor != '\0'; ++cursor) {
    digest = gas_schedule_digest_byte(digest, static_cast<uint8_t>(*cursor));
  }
  return gas_schedule_digest_byte(digest, 0);
}

constexpr uint64_t
gas_schedule_digest()
{
  uint64_t digest = kFnvOffsetBasis;
  digest = gas_schedule_digest_u64(digest, kGasTableVersion);
  digest = gas_schedule_digest_name(digest, "rv32-instructions");
  digest = gas_schedule_digest_u64(digest, kOpCount);
  for (size_t i = 0; i < kInstructionGasSchedule.size(); ++i) {
    const auto &entry = kInstructionGasSchedule[i];
    digest = gas_schedule_digest_u64(digest, i);
    digest = gas_schedule_digest_u64(digest, static_cast<size_t>(entry.op));
    digest = gas_schedule_digest_u64(digest, entry.cost);
    digest = gas_schedule_digest_name(digest, entry.name);
  }
  digest = gas_schedule_digest_name(digest, "host-syscalls");
  digest =
      gas_schedule_digest_u64(digest, hogd::kSyscallGasCostSchedule.size());
  for (size_t i = 0; i < hogd::kSyscallGasCostSchedule.size(); ++i) {
    const auto &entry = hogd::kSyscallGasCostSchedule[i];
    digest = gas_schedule_digest_u64(digest, i);
    digest = gas_schedule_digest_u64(digest, entry.cost);
    digest = gas_schedule_digest_name(digest, entry.name);
  }
  return digest;
}

inline constexpr uint64_t kGasScheduleDigest = gas_schedule_digest();

// Relocating this computation from program.h must not change its value (it is
// validated against every artifact). Pin it.
static_assert(kGasScheduleDigest == 0xebe53dceea581d0cULL);

using ArtifactDigest = std::array<uint8_t, kArtifactDigestBytes>;

// RV32 artifact v3 header, little-endian:
// magic[8], format_version, header_bytes, vm_profile_digest, flags,
// entry_abi_version, syscall_abi_version, gas_schedule_version,
// gas_schedule_digest, memory_bytes, code_size, initial_memory_size,
// code_offset, initial_memory_offset, dispatch_table_offset,
// dispatch_table_size, sha256(code), sha256(initial_memory),
// sha256(dispatch).
enum class ArtifactError {
  kTooSmall,
  kBadMagic,
  kUnsupportedFormatVersion,
  kBadHeaderSize,
  kUnsupportedVmProfile,
  kUnsupportedFlags,
  kUnsupportedEntryAbi,
  kUnsupportedSyscallAbi,
  kGasScheduleVersionMismatch,
  kGasScheduleDigestMismatch,
  kMemorySizeInvalid,
  kCodeSizeInvalid,
  kInitialMemorySizeMismatch,
  kInitialMemoryTooLarge,
  kInvalidOffsets,
  kDispatchSizeInvalid,
  kDispatchDigestMismatch,
  kDispatchTableInvalid,
  kCodeDigestMismatch,
  kInitialMemoryDigestMismatch,
  kInvalidProgram,
};

struct ArtifactImage {
  std::shared_ptr<const Program> program;
  std::vector<uint8_t> initial_memory;
  std::vector<DispatchEntry> dispatch_table;
  uint32_t memory_bytes = kMaxMemoryBytes;
};

[[nodiscard]]
ArtifactDigest artifact_digest(std::span<const uint8_t> bytes);

[[nodiscard]]
std::expected<ArtifactImage, ArtifactError>
parse_artifact(std::span<const uint8_t> bytes);

} // namespace hogd::rv32
