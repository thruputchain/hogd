#include "hogd/hogexec/runtime/rv32/artifact.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace hogd::rv32 {
namespace {

uint32_t
load_u32(std::span<const uint8_t> bytes, size_t offset)
{
  return uint32_t{bytes[offset]} | (uint32_t{bytes[offset + 1]} << 8) |
         (uint32_t{bytes[offset + 2]} << 16) |
         (uint32_t{bytes[offset + 3]} << 24);
}

uint64_t
load_u64(std::span<const uint8_t> bytes, size_t offset)
{
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    value |= uint64_t{bytes[offset + i]} << (i * 8);
  }
  return value;
}

bool
range_within(uint32_t offset, uint32_t size, size_t total)
{
  return size_t{offset} <= total && size_t{size} <= total - size_t{offset};
}

bool
ranges_overlap(uint32_t left_offset, uint32_t left_size,
               uint32_t right_offset, uint32_t right_size)
{
  const size_t left_begin = left_offset;
  const size_t left_end = left_begin + left_size;
  const size_t right_begin = right_offset;
  const size_t right_end = right_begin + right_size;
  return left_begin < right_end && right_begin < left_end;
}

} // namespace

uint64_t
artifact_digest(std::span<const uint8_t> bytes)
{
  uint64_t digest = 1469598103934665603ULL;
  digest = gas_schedule_digest_u64(digest, bytes.size());
  for (const uint8_t byte : bytes) {
    digest = gas_schedule_digest_byte(digest, byte);
  }
  return digest;
}

std::expected<ArtifactImage, ArtifactError>
parse_artifact(std::span<const uint8_t> bytes)
{
  if (bytes.size() < kArtifactHeaderBytes) {
    return std::unexpected(ArtifactError::kTooSmall);
  }
  if (!std::ranges::equal(kArtifactMagic, bytes.first(kArtifactMagic.size()))) {
    return std::unexpected(ArtifactError::kBadMagic);
  }

  const uint32_t format_version = load_u32(bytes, 8);
  const uint32_t header_size = load_u32(bytes, 12);
  const uint64_t vm_profile_digest = load_u64(bytes, 16);
  const uint32_t flags = load_u32(bytes, 24);
  const uint32_t entry_abi_version = load_u32(bytes, 28);
  const uint32_t syscall_abi_version = load_u32(bytes, 32);
  const uint32_t gas_schedule_version = load_u32(bytes, 36);
  const uint64_t gas_digest = load_u64(bytes, 40);
  const uint32_t memory_bytes = load_u32(bytes, 48);
  const uint32_t code_size = load_u32(bytes, 52);
  const uint32_t initial_memory_size = load_u32(bytes, 56);
  const uint32_t code_offset = load_u32(bytes, 60);
  const uint32_t initial_memory_offset = load_u32(bytes, 64);
  const uint32_t dispatch_table_offset = load_u32(bytes, 68);
  const uint32_t dispatch_table_size = load_u32(bytes, 72);
  const uint64_t code_digest = load_u64(bytes, 76);
  const uint64_t initial_memory_digest = load_u64(bytes, 84);

  if (format_version != kArtifactFormatVersion) {
    return std::unexpected(ArtifactError::kUnsupportedFormatVersion);
  }
  if (header_size != kArtifactHeaderBytes) {
    return std::unexpected(ArtifactError::kBadHeaderSize);
  }
  if (vm_profile_digest != kArtifactVmProfileDigest) {
    return std::unexpected(ArtifactError::kUnsupportedVmProfile);
  }
  if (flags != 0) {
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
  if (initial_memory_size > memory_bytes) {
    return std::unexpected(ArtifactError::kInitialMemoryTooLarge);
  }
  if (initial_memory_size != memory_bytes) {
    return std::unexpected(ArtifactError::kInitialMemorySizeMismatch);
  }
  if (code_offset < header_size || initial_memory_offset < header_size ||
      !range_within(code_offset, code_size, bytes.size()) ||
      !range_within(initial_memory_offset, initial_memory_size, bytes.size()) ||
      ranges_overlap(code_offset, code_size, initial_memory_offset,
                     initial_memory_size)) {
    return std::unexpected(ArtifactError::kInvalidOffsets);
  }
  const size_t code_end = size_t{code_offset} + code_size;
  const size_t initial_memory_end =
      size_t{initial_memory_offset} + initial_memory_size;
  if (code_offset != header_size || initial_memory_offset != code_end ||
      initial_memory_end != bytes.size()) {
    return std::unexpected(ArtifactError::kInvalidOffsets);
  }
  if (dispatch_table_offset != 0 || dispatch_table_size != 0) {
    return std::unexpected(ArtifactError::kDispatchUnsupported);
  }

  const auto code = bytes.subspan(code_offset, code_size);
  const auto initial_memory =
      bytes.subspan(initial_memory_offset, initial_memory_size);
  if (artifact_digest(code) != code_digest) {
    return std::unexpected(ArtifactError::kCodeDigestMismatch);
  }
  if (artifact_digest(initial_memory) != initial_memory_digest) {
    return std::unexpected(ArtifactError::kInitialMemoryDigestMismatch);
  }

  auto program = Program::create(code);
  if (!program.has_value()) {
    return std::unexpected(ArtifactError::kInvalidProgram);
  }

  return ArtifactImage{
      .program = std::make_shared<Program>(std::move(*program)),
      .initial_memory =
          std::vector<uint8_t>(initial_memory.begin(), initial_memory.end()),
      .memory_bytes = memory_bytes,
  };
}

} // namespace hogd::rv32
