#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/dispatch.h"
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
