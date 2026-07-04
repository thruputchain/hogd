#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

// `u8'...'` character literals so the on-the-wire artifact magic is the UTF-8
// code unit of each character, independent of the ordinary execution character
// set (`-fexec-charset`). An ordinary `'G'` would instead be an
// implementation-defined byte. See hogd issue #2689.
inline constexpr std::array<uint8_t, 8> kArtifactMagic = {u8'G', u8'H', u8'R',
                                                          u8'V', u8'3', u8'2',
                                                          u8'A', u8'1'};
inline constexpr uint32_t kArtifactFormatVersion = 5;
inline constexpr uint32_t kArtifactHeaderBytes = 128;
inline constexpr uint64_t kArtifactVmProfileDigest = 0x55c53a685f0eb33dULL;
// v3: the VM seeds sp (x2) to 0 at entry; the guest owns stack placement and
// must set sp itself (a stack touch before that wraps below 0 and traps OOB).
// v2 seeded sp to the top of data memory.
inline constexpr uint32_t kArtifactEntryAbiVersion = 3;
// v1: register-native syscall ABI (args in a0..a7, no memory frame/footer).
inline constexpr uint32_t kArtifactSyscallAbiVersion = 1;
inline constexpr uint32_t kArtifactFlagsRequiredValue = 0;
// Version of the RV32 gas schedule (instruction + host-syscall costs) stamped
// into the header at kArtifactGasVersionOffset and validated at load.
// v3 admits canonical UNIMP (0xc0001073) as a deterministic invalid-instruction
// trap.
inline constexpr uint32_t kGasTableVersion = 3;
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
inline constexpr size_t kArtifactInitialMemoryStartOffset = 60;
inline constexpr size_t kArtifactCodeDigestOffset = 64;
inline constexpr size_t kArtifactInitialMemoryDigestOffset =
    kArtifactCodeDigestOffset + kArtifactDigestBytes;
inline constexpr char8_t kArtifactSectionDigestAlgorithm[] = u8"sha256";
inline constexpr char8_t kArtifactCanonicalSectionOrder[] =
    u8"header|code|initial_memory";

static_assert(kArtifactInitialMemoryDigestOffset + kArtifactDigestBytes ==
              kArtifactHeaderBytes);

// FNV-1a digest over the full RV32 gas schedule: the instruction-cost table
// (from program.h) folded with the host-syscall cost schedule (from
// gas_cost_table.h). It is stamped into the artifact at
// kArtifactGasDigestOffset and re-derived here at load time, so an artifact
// built against a different schedule is rejected. This is an artifact/consensus
// concern, not an ISA-execution one, so it lives here rather than in the
// portable program.h core (which only needs the instruction costs themselves).
constexpr uint64_t
gas_schedule_digest()
{
  uint64_t digest = kFnvOffsetBasis;
  digest = fnv_digest_u64(digest, kGasTableVersion);
  digest = fnv_digest_name(digest, u8"rv32-instructions");
  digest = fnv_digest_u64(digest, kOpCount);
  for (size_t i = 0; i < kInstructionGasSchedule.size(); ++i) {
    const auto &entry = kInstructionGasSchedule[i];
    digest = fnv_digest_u64(digest, i);
    digest = fnv_digest_u64(digest, static_cast<size_t>(entry.op));
    digest = fnv_digest_u64(digest, entry.cost);
    digest = fnv_digest_name(digest, entry.name);
  }
  digest = fnv_digest_name(digest, u8"host-syscalls");
  digest = fnv_digest_u64(digest, hogd::kSyscallGasCostSchedule.size());
  for (size_t i = 0; i < hogd::kSyscallGasCostSchedule.size(); ++i) {
    const auto &entry = hogd::kSyscallGasCostSchedule[i];
    digest = fnv_digest_u64(digest, i);
    digest = fnv_digest_u64(digest, entry.cost);
    digest = fnv_digest_name(digest, entry.name);
  }
  return digest;
}

inline constexpr uint64_t kGasScheduleDigest = gas_schedule_digest();

// Relocating this computation from program.h must not change its value (it is
// validated against every artifact). Pin it.
static_assert(kGasScheduleDigest == 0x918317c21c46bb60ULL);

constexpr uint64_t
format_fingerprint_u64(uint64_t digest, const char8_t *name, uint64_t value)
{
  return fnv_digest_u64(fnv_digest_name(digest, name), value);
}

template<size_t N>
constexpr uint64_t
format_fingerprint_bytes(uint64_t digest, const char8_t *name,
                         const std::array<uint8_t, N> &bytes)
{
  digest = fnv_digest_name(digest, name);
  digest = fnv_digest_u64(digest, N);
  for (uint8_t byte : bytes) {
    digest = fnv_digest_byte(digest, byte);
  }
  return digest;
}

template<size_t N>
constexpr uint64_t
format_fingerprint_string(uint64_t digest, const char8_t *name,
                          const char8_t (&value)[N])
{
  static_assert(N > 0);
  digest = fnv_digest_name(digest, name);
  digest = fnv_digest_u64(digest, N - 1);
  for (size_t i = 0; i < N - 1; ++i) {
    digest = fnv_digest_byte(digest, static_cast<uint8_t>(value[i]));
  }
  return digest;
}

constexpr uint64_t
format_fingerprint_layout_field(uint64_t digest, const char8_t *offset_name,
                                const char8_t *size_name, uint64_t offset,
                                uint64_t size)
{
  digest = format_fingerprint_u64(digest, offset_name, offset);
  return format_fingerprint_u64(digest, size_name, size);
}

// The `format_version` parameter is a sensitivity-test hook only (the
// static_assert below perturbs it to prove the digest reacts), NOT a
// version-dispatch mechanism: the domain string below is hard-wired to the
// current format version. At a format bump, bump BOTH the domain string
// (`...format.vN`) and kArtifactFormatVersion together — passing a different
// format_version here does not produce a correct fingerprint for another
// version on its own.
constexpr uint64_t
format_fingerprint(uint32_t format_version = kArtifactFormatVersion)
{
  uint64_t digest = kFnvOffsetBasis;
  digest = fnv_digest_name(digest, u8"groundhog.rv32.artifact.format.v5");

  digest = format_fingerprint_bytes(digest, u8"magic", kArtifactMagic);
  digest = format_fingerprint_u64(digest, u8"format_version", format_version);
  digest =
      format_fingerprint_u64(digest, u8"header_bytes", kArtifactHeaderBytes);
  digest = format_fingerprint_u64(digest, u8"vm_profile_digest",
                                  kArtifactVmProfileDigest);
  digest = format_fingerprint_u64(digest, u8"flags_required_value",
                                  kArtifactFlagsRequiredValue);
  digest = format_fingerprint_u64(digest, u8"entry_abi_version",
                                  kArtifactEntryAbiVersion);
  digest = format_fingerprint_u64(digest, u8"syscall_abi_version",
                                  kArtifactSyscallAbiVersion);
  digest =
      format_fingerprint_u64(digest, u8"gas_table_version", kGasTableVersion);
  digest = format_fingerprint_u64(digest, u8"gas_schedule_digest",
                                  kGasScheduleDigest);

  digest = format_fingerprint_layout_field(digest, u8"header.magic.offset",
                                           u8"header.magic.size", 0,
                                           kArtifactMagic.size());
  digest =
      format_fingerprint_layout_field(digest, u8"header.format_version.offset",
                                      u8"header.format_version.size",
                                      kArtifactFormatVersionOffset, 4);
  digest =
      format_fingerprint_layout_field(digest, u8"header.header_size.offset",
                                      u8"header.header_size.size",
                                      kArtifactHeaderSizeOffset, 4);
  digest = format_fingerprint_layout_field(digest, u8"header.vm_profile.offset",
                                           u8"header.vm_profile.size",
                                           kArtifactVmProfileOffset, 8);
  digest = format_fingerprint_layout_field(digest, u8"header.flags.offset",
                                           u8"header.flags.size",
                                           kArtifactFlagsOffset, 4);
  digest = format_fingerprint_layout_field(digest, u8"header.entry_abi.offset",
                                           u8"header.entry_abi.size",
                                           kArtifactEntryAbiOffset, 4);
  digest =
      format_fingerprint_layout_field(digest, u8"header.syscall_abi.offset",
                                      u8"header.syscall_abi.size",
                                      kArtifactSyscallAbiOffset, 4);
  digest =
      format_fingerprint_layout_field(digest, u8"header.gas_version.offset",
                                      u8"header.gas_version.size",
                                      kArtifactGasVersionOffset, 4);
  digest = format_fingerprint_layout_field(digest, u8"header.gas_digest.offset",
                                           u8"header.gas_digest.size",
                                           kArtifactGasDigestOffset, 8);
  digest =
      format_fingerprint_layout_field(digest, u8"header.memory_bytes.offset",
                                      u8"header.memory_bytes.size",
                                      kArtifactMemoryBytesOffset, 4);
  digest = format_fingerprint_layout_field(digest, u8"header.code_size.offset",
                                           u8"header.code_size.size",
                                           kArtifactCodeSizeOffset, 4);
  digest =
      format_fingerprint_layout_field(digest,
                                      u8"header.initial_memory_size.offset",
                                      u8"header.initial_memory_size.size",
                                      kArtifactInitialMemorySizeOffset, 4);
  digest =
      format_fingerprint_layout_field(digest,
                                      u8"header.initial_memory_start.offset",
                                      u8"header.initial_memory_start.size",
                                      kArtifactInitialMemoryStartOffset, 4);
  digest =
      format_fingerprint_layout_field(digest, u8"header.code_digest.offset",
                                      u8"header.code_digest.size",
                                      kArtifactCodeDigestOffset,
                                      kArtifactDigestBytes);
  digest =
      format_fingerprint_layout_field(digest,
                                      u8"header.initial_memory_digest.offset",
                                      u8"header.initial_memory_digest.size",
                                      kArtifactInitialMemoryDigestOffset,
                                      kArtifactDigestBytes);

  digest = format_fingerprint_string(digest, u8"section_digest_algorithm",
                                     kArtifactSectionDigestAlgorithm);
  digest =
      format_fingerprint_u64(digest, u8"digest_bytes", kArtifactDigestBytes);
  digest =
      format_fingerprint_u64(digest, u8"instruction_bytes", kInstructionBytes);
  digest = format_fingerprint_u64(digest, u8"max_code_bytes", kMaxCodeBytes);
  digest =
      format_fingerprint_u64(digest, u8"max_memory_bytes", kMaxMemoryBytes);

  digest = format_fingerprint_u64(digest, u8"memory_bytes_nonzero", 1);
  digest = format_fingerprint_u64(digest, u8"memory_bytes_le_max", 1);
  digest = format_fingerprint_u64(digest, u8"code_size_nonzero", 1);
  digest = format_fingerprint_u64(
      digest, u8"initial_memory_start_plus_size_le_memory_bytes", 1);
  digest = format_fingerprint_u64(digest,
                                  u8"initial_memory_start_zero_when_empty", 1);
  digest = format_fingerprint_string(digest, u8"canonical_section_order",
                                     kArtifactCanonicalSectionOrder);
  digest =
      format_fingerprint_u64(digest, u8"no_section_gaps_or_trailing_bytes", 1);
  return digest;
}

inline constexpr uint64_t kArtifactFormatFingerprint = format_fingerprint();
static_assert(kArtifactFormatFingerprint == 0x8e5caf3e9cd23b22ULL);
static_assert(format_fingerprint(kArtifactFormatVersion + 1) !=
              kArtifactFormatFingerprint);

using ArtifactDigest = std::array<uint8_t, kArtifactDigestBytes>;

// RV32 artifact v5 header, little-endian:
// magic[8], format_version, header_bytes, vm_profile_digest, flags,
// entry_abi_version, syscall_abi_version, gas_schedule_version,
// gas_schedule_digest, memory_bytes, code_size, initial_memory_size,
// initial_memory_start, sha256(code), sha256(initial_memory).
// Body: header || code || initial_memory, exactly — section file positions
// are derived from the canonical order, not stored. Method dispatch is in the
// guest (since v4). The stored image is placed at data-space VA
// initial_memory_start (a guest VA, unrelated to file layout); the runtime
// zero-fills [0, start) — e.g. a stack-first guest's stack region — and
// [start + size, memory_bytes).
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
  kInitialMemoryTooLarge,
  kInvalidSectionLayout,
  kCodeDigestMismatch,
  kInitialMemoryDigestMismatch,
  kInvalidProgram,
};

struct ArtifactImage {
  std::shared_ptr<const Program> program;
  // Materialized prefix [0, initial_memory_start + stored size): leading
  // zeros below the image's load VA are included so consumers can keep
  // writing it at data address 0.
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = kMaxMemoryBytes;
};

// The ThreadedArtifactImage output from parse_threaded_artifact must not
// outlive the input span.
struct ThreadedArtifactImage {
  std::span<const uint8_t> code;
  std::span<const uint8_t> initial_memory;
  // Data-space VA where initial_memory is placed; [0, start) zero-fills.
  uint32_t initial_memory_start = 0;
  uint32_t memory_bytes = kMaxMemoryBytes;
};

[[nodiscard]]
ArtifactDigest artifact_digest(std::span<const uint8_t> bytes);

[[nodiscard]]
std::expected<ArtifactImage, ArtifactError>
parse_artifact(std::span<const uint8_t> bytes);

[[nodiscard]]
std::expected<ThreadedArtifactImage, ArtifactError>
parse_threaded_artifact(std::span<const uint8_t> bytes);

} // namespace hogd::rv32
