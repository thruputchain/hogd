#include "hogd/hogexec/runtime/rv32/artifact.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <openssl/sha.h>

#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {
namespace {

ArtifactDigest
load_artifact_digest(std::span<const uint8_t> bytes, size_t offset)
{
  ArtifactDigest digest = {};
  std::ranges::copy(bytes.subspan(offset, kArtifactDigestBytes),
                    digest.begin());
  return digest;
}

struct ArtifactParseResults {
  std::span<const uint8_t> code;
  std::span<const uint8_t> initial_memory;
  uint32_t initial_memory_start;
  uint32_t total_memory_bytes;
};

[[nodiscard]]
std::expected<ArtifactParseResults, ArtifactError>
artifact_image_checks(std::span<const uint8_t> bytes)
{

  if (bytes.size() < kArtifactHeaderBytes) {
    return std::unexpected(ArtifactError::kTooSmall);
  }
  if (!std::ranges::equal(kArtifactMagic, bytes.first(kArtifactMagic.size()))) {
    return std::unexpected(ArtifactError::kBadMagic);
  }

  const uint32_t format_version =
      load_u32_le(bytes, kArtifactFormatVersionOffset);
  const uint32_t header_size = load_u32_le(bytes, kArtifactHeaderSizeOffset);
  const uint64_t vm_profile_digest =
      load_u64_le(bytes, kArtifactVmProfileOffset);
  const uint32_t flags = load_u32_le(bytes, kArtifactFlagsOffset);
  const uint32_t entry_abi_version =
      load_u32_le(bytes, kArtifactEntryAbiOffset);
  const uint32_t syscall_abi_version =
      load_u32_le(bytes, kArtifactSyscallAbiOffset);
  const uint32_t gas_schedule_version =
      load_u32_le(bytes, kArtifactGasVersionOffset);
  const uint64_t gas_digest = load_u64_le(bytes, kArtifactGasDigestOffset);
  const uint32_t memory_bytes = load_u32_le(bytes, kArtifactMemoryBytesOffset);
  const uint32_t code_size = load_u32_le(bytes, kArtifactCodeSizeOffset);
  const uint32_t initial_memory_size =
      load_u32_le(bytes, kArtifactInitialMemorySizeOffset);
  const uint32_t initial_memory_start =
      load_u32_le(bytes, kArtifactInitialMemoryStartOffset);
  const ArtifactDigest code_digest =
      load_artifact_digest(bytes, kArtifactCodeDigestOffset);
  const ArtifactDigest initial_memory_digest =
      load_artifact_digest(bytes, kArtifactInitialMemoryDigestOffset);

  if (format_version != kArtifactFormatVersion) {
    return std::unexpected(ArtifactError::kUnsupportedFormatVersion);
  }
  if (header_size != kArtifactHeaderBytes) {
    return std::unexpected(ArtifactError::kBadHeaderSize);
  }
  if (vm_profile_digest != kArtifactVmProfileDigest) {
    return std::unexpected(ArtifactError::kUnsupportedVmProfile);
  }
  if (flags != kArtifactFlagsRequiredValue) {
    return std::unexpected(ArtifactError::kUnsupportedFlags);
  }
  if (entry_abi_version != kArtifactEntryAbiVersion) {
    return std::unexpected(ArtifactError::kUnsupportedEntryAbi);
  }
  if (syscall_abi_version != kArtifactSyscallAbiVersion) {
    return std::unexpected(ArtifactError::kUnsupportedSyscallAbi);
  }
  if (gas_schedule_version != kGasTableVersion) {
    return std::unexpected(ArtifactError::kGasScheduleVersionMismatch);
  }
  if (gas_digest != kGasScheduleDigest) {
    return std::unexpected(ArtifactError::kGasScheduleDigestMismatch);
  }
  if (memory_bytes == 0 || memory_bytes > kMaxMemoryBytes) {
    return std::unexpected(ArtifactError::kMemorySizeInvalid);
  }
  if (code_size == 0 || code_size > kMaxCodeBytes) {
    return std::unexpected(ArtifactError::kCodeSizeInvalid);
  }

  // The stored image is placed at [start, start + size) in data space; the
  // runtime zero-fills [0, start) and [start + size, memory_bytes).
  if (size_t{initial_memory_start} + initial_memory_size > memory_bytes) {
    return std::unexpected(ArtifactError::kInitialMemoryTooLarge);
  }
  // Canonicality: an empty image has exactly one encoding.
  if (initial_memory_size == 0 && initial_memory_start != 0) {
    return std::unexpected(ArtifactError::kInitialMemoryTooLarge);
  }
  // Canonical v5 layout: header || code || initial_memory, exactly. Section
  // positions are derived, so the only structural degree of freedom left is
  // total size.
  if (bytes.size() !=
      size_t{kArtifactHeaderBytes} + code_size + initial_memory_size) {
    return std::unexpected(ArtifactError::kInvalidSectionLayout);
  }

  const auto code = bytes.subspan(kArtifactHeaderBytes, code_size);
  const auto initial_memory =
      bytes.subspan(size_t{kArtifactHeaderBytes} + code_size,
                    initial_memory_size);
  if (artifact_digest(code) != code_digest) {
    return std::unexpected(ArtifactError::kCodeDigestMismatch);
  }
  if (artifact_digest(initial_memory) != initial_memory_digest) {
    return std::unexpected(ArtifactError::kInitialMemoryDigestMismatch);
  }

  return ArtifactParseResults{
      .code = code,
      .initial_memory = initial_memory,
      .initial_memory_start = initial_memory_start,
      .total_memory_bytes = memory_bytes,
  };
}

} // namespace

ArtifactDigest
artifact_digest(std::span<const uint8_t> bytes)
{
  ArtifactDigest digest = {};
  // Keep rv32_vm dependency-narrow for now. Protocol activation should either
  // use the consensus SHA256 wrapper or keep an explicit equivalence check.
  static constexpr std::array<uint8_t, 1> kEmpty = {0};
  // OpenSSL ignores the input pointer when len is zero; provide a non-null
  // placeholder so the call shape remains valid for empty sections too.
  const uint8_t *input = bytes.empty() ? kEmpty.data() : bytes.data();
  SHA256(input, bytes.size(), digest.data());
  return digest;
}

std::expected<ArtifactImage, ArtifactError>
parse_artifact(std::span<const uint8_t> bytes)
{
  auto results = artifact_image_checks(bytes);
  if (!results.has_value()) {
    return std::unexpected(results.error());
  }

  auto program = Program::create(results->code);
  if (!program.has_value()) {
    return std::unexpected(ArtifactError::kInvalidProgram);
  }

  std::vector<uint8_t> initial_memory(size_t{results->initial_memory_start} +
                                      results->initial_memory.size());
  std::ranges::copy(results->initial_memory,
                    initial_memory.begin() + results->initial_memory_start);

  return ArtifactImage{
      .program = std::make_shared<Program>(std::move(*program)),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = results->total_memory_bytes,
  };
}

std::expected<ThreadedArtifactImage, ArtifactError>
parse_threaded_artifact(std::span<const uint8_t> bytes)
{
  auto results = artifact_image_checks(bytes);
  if (!results.has_value()) {
    return std::unexpected(results.error());
  }

  return ThreadedArtifactImage{
      .code = results->code,
      .initial_memory = results->initial_memory,
      .initial_memory_start = results->initial_memory_start,
      .memory_bytes = results->total_memory_bytes,
  };
}

} // namespace hogd::rv32
