#include "hogd/hogexec/runtime/rv32/artifact.h"

#include <algorithm>
#include <memory>
#include <span>
#include <utility>

namespace hogd::rv32 {
namespace {

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
  uint64_t digest = kFnvOffsetBasis;
  digest = fnv_digest_u64(digest, bytes.size());
  for (const uint8_t byte : bytes) {
    digest = fnv_digest_byte(digest, byte);
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
  const uint32_t code_offset = load_u32_le(bytes, kArtifactCodeOffsetOffset);
  const uint32_t initial_memory_offset =
      load_u32_le(bytes, kArtifactInitialMemoryOffsetOffset);
  const uint32_t dispatch_table_offset =
      load_u32_le(bytes, kArtifactDispatchOffsetOffset);
  const uint32_t dispatch_table_size =
      load_u32_le(bytes, kArtifactDispatchSizeOffset);
  const uint64_t code_digest = load_u64_le(bytes, kArtifactCodeDigestOffset);
  const uint64_t initial_memory_digest =
      load_u64_le(bytes, kArtifactInitialMemoryDigestOffset);

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
  // M4 stores a full memory image so each loaded VM state has exactly one
  // valid artifact byte encoding. Sparse sections can be a later format.
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
  // Keep the v1 layout canonical: header, code, memory, and no hidden bytes.
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
