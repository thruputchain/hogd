#include "hogd/hog_exec/runtime/rv32/artifact.h"

#include <algorithm>
#include <array>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <openssl/sha.h>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/rv32/encoding.h"
#include "hogd/hog_exec/runtime/rv32/program.h"

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

uint32_t
load_u32_be(std::span<const uint8_t> bytes, size_t offset)
{
  return (uint32_t{bytes[offset]} << 24) | (uint32_t{bytes[offset + 1]} << 16) |
         (uint32_t{bytes[offset + 2]} << 8) | uint32_t{bytes[offset + 3]};
}

// Whether `bytes` is exactly one XDR Rv32ArtifactExtensions (contract.x) with
// nonzero, strictly ascending kinds and zero padding. Bodies are not read.
[[nodiscard]]
bool
canonical_extension_list(std::span<const uint8_t> bytes)
{
  if (bytes.size() < 4) {
    return false;
  }
  const uint32_t count = load_u32_be(bytes, 0);
  size_t offset = 4;
  uint32_t previous_kind = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (bytes.size() - offset < 8) {
      return false;
    }
    const uint32_t kind = load_u32_be(bytes, offset);
    const uint32_t body_size = load_u32_be(bytes, offset + 4);
    offset += 8;
    if (kind <= previous_kind) {
      return false;
    }
    previous_kind = kind;
    // Widen before padding so a crafted size cannot wrap on 32-bit hosts.
    const uint64_t padded = (uint64_t{body_size} + 3) & ~uint64_t{3};
    if (bytes.size() - offset < padded) {
      return false;
    }
    const auto padding = bytes.subspan(offset + body_size,
                                       static_cast<size_t>(padded) - body_size);
    if (std::ranges::any_of(padding, [](uint8_t b) { return b != 0; })) {
      return false;
    }
    offset += static_cast<size_t>(padded);
  }
  return offset == bytes.size();
}

struct ArtifactParseResults {
  std::span<const uint8_t> code;
  std::span<const uint8_t> initial_memory;
  uint32_t initial_memory_start;
  uint32_t total_memory_bytes;
};

[[nodiscard]]
std::expected<ArtifactParseResults, ArtifactError>
artifact_image_checks(std::span<const uint8_t> bytes, Rv32Limits limits)
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
  const bool legacy = format_version == kLegacyArtifactFormatVersion;
  if (!legacy && format_version != kArtifactFormatVersion) {
    return std::unexpected(ArtifactError::kUnsupportedFormatVersion);
  }
  if (header_size != kArtifactHeaderBytes) {
    return std::unexpected(ArtifactError::kBadHeaderSize);
  }
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
  // Accepted as a (version, digest) pair; see kAcceptedGasSchedules for why the
  // list can hold more than the current schedule and when to shrink it.
  const auto *accepted =
      std::ranges::find(kAcceptedGasSchedules, gas_schedule_version,
                        &AcceptedGasSchedule::version);
  if (accepted == kAcceptedGasSchedules.end()) {
    return std::unexpected(ArtifactError::kGasScheduleVersionMismatch);
  }
  if (gas_digest != accepted->digest) {
    return std::unexpected(ArtifactError::kGasScheduleDigestMismatch);
  }
  // memory_bytes == 0 is a valid degenerate: a zero-memory contract whose
  // data accesses all fault. Only the upper bound is enforced, and it is the
  // caller's: the chain's configuration at attachment, the VM's ceiling for a
  // deployed contract.
  if (memory_bytes > limits.max_memory_bytes) {
    return std::unexpected(ArtifactError::kMemorySizeInvalid);
  }
  if (code_size == 0 || code_size > limits.max_code_bytes) {
    return std::unexpected(ArtifactError::kCodeSizeInvalid);
  }

  // The stored image is placed at [start, start + size) in data space; the
  // runtime zero-fills [0, start) and [start + size, memory_bytes). Check the
  // fit with subtraction so the guard is independent of host pointer width:
  // start + size can wrap when size_t is 32 bits.
  if (initial_memory_start > memory_bytes ||
      initial_memory_size > memory_bytes - initial_memory_start) {
    return std::unexpected(ArtifactError::kInitialMemoryTooLarge);
  }
  // Canonicality: an empty image has exactly one encoding.
  if (initial_memory_size == 0 && initial_memory_start != 0) {
    return std::unexpected(ArtifactError::kInitialMemoryTooLarge);
  }
  // header || code || initial_memory, then (v6 only) the extension list to
  // the end. Extension bodies are opaque to execution and bounded only by the
  // total contract size. Widen before adding so crafted u32 lengths cannot
  // wrap on 32-bit hosts.
  const uint64_t sections_end =
      uint64_t{header_size} + code_size + initial_memory_size;
  if (bytes.size() < sections_end) {
    return std::unexpected(ArtifactError::kInvalidSectionLayout);
  }
  const auto extensions = bytes.subspan(static_cast<size_t>(sections_end));
  if (legacy ? !extensions.empty() : !canonical_extension_list(extensions)) {
    return std::unexpected(ArtifactError::kInvalidSectionLayout);
  }

  const auto code = bytes.subspan(header_size, code_size);
  const auto initial_memory =
      bytes.subspan(size_t{header_size} + code_size, initial_memory_size);
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
  // Use the same SHA-256 primitives as SHA256Hasher, without its dependencies.
  // OpenSSL 3's one-shot API can fail during provider lookup and leave the
  // output untouched (hogd #2696). These primitives avoid provider lookup.
  // Failure is a local crypto failure, not an invalid artifact: stop the node
  // rather than admit or reject artifacts using an uncomputed digest.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  ArtifactDigest digest = {};
  SHA256_CTX ctx;
  // Avoid passing a null data pointer for an empty section.
  const bool ok =
      SHA256_Init(&ctx) == 1 &&
      (bytes.empty() || SHA256_Update(&ctx, bytes.data(), bytes.size()) == 1) &&
      SHA256_Final(digest.data(), &ctx) == 1;
#pragma GCC diagnostic pop
  CHECK(ok);
  return digest;
}

std::expected<ArtifactImage, ArtifactError>
parse_artifact(std::span<const uint8_t> bytes, Rv32Limits limits)
{
  auto results = artifact_image_checks(bytes, limits);
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
parse_threaded_artifact(std::span<const uint8_t> bytes, Rv32Limits limits)
{
  auto results = artifact_image_checks(bytes, limits);
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
