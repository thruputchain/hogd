#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

inline constexpr std::array<uint8_t, 8> kArtifactMagic = {
    'G', 'H', 'R', 'V', '3', '2', 'A', '1'};
inline constexpr uint32_t kArtifactFormatVersion = 1;
inline constexpr uint32_t kArtifactHeaderBytes = 92;
inline constexpr uint64_t kArtifactVmProfileDigest = 0x55c53a685f0eb33dULL;
inline constexpr uint32_t kArtifactEntryAbiVersion = 0;
inline constexpr uint32_t kArtifactSyscallAbiVersion = 0;

// Experimental M4 artifact header, little-endian:
// magic[8], format_version, header_bytes, vm_profile_digest, flags,
// entry_abi_version, syscall_abi_version, gas_schedule_version,
// gas_schedule_digest, memory_bytes, code_size, initial_memory_size,
// code_offset, initial_memory_offset, dispatch_table_offset,
// dispatch_table_size, code_digest, initial_memory_digest.
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
  kDispatchUnsupported,
  kCodeDigestMismatch,
  kInitialMemoryDigestMismatch,
  kInvalidProgram,
};

struct ArtifactImage {
  std::shared_ptr<const Program> program;
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = kMaxMemoryBytes;
};

// Short deterministic checksum for internal M4 validation. This is not a
// protocol-grade content commitment.
uint64_t artifact_digest(std::span<const uint8_t> bytes);

std::expected<ArtifactImage, ArtifactError>
parse_artifact(std::span<const uint8_t> bytes);

} // namespace hogd::rv32
