#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/encoding.h"
#include "hogd/hog_exec/runtime/rv32/fixtures/compiled_sha256_return/compiled_sha256_return_fixture.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/machine.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_support.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

// Some legacy VM tests keep descriptive snake_case names and compact aggregate
// tables; the checks still run through clang-tidy for the production code
// paths.
// NOLINTBEGIN(google-readability-avoid-underscore-in-googletest-name,modernize-use-designated-initializers,readability-suspicious-call-argument,bugprone-random-generator-seed,bugprone-unchecked-optional-access)

namespace hogd::rv32 {
namespace {

namespace compiled_sha256_return = fixtures::compiled_sha256_return;
using test_program::addi;
using test_program::append_u32;
using test_program::b_type;
using test_program::code_from_words;
using test_program::i_type;
using test_program::j_type;
using test_program::jalr;
using test_program::load;
using test_program::make_program;
using test_program::r_type;
using test_program::s_type;
using test_program::u_type;

// Mutation-test base: a minimal valid artifact from the test builder, so
// format-validation tests track the current format constants instead of the
// compiled fixture's vintage.
std::vector<uint8_t>
valid_artifact_bytes()
{
  static constexpr std::array<uint8_t, 4> kStored = {1, 2, 3, 4};
  return test_artifact::build_artifact({addi(0, 0, 0), 0x00000073U}, kStored);
}

void
store_u32_at(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void
store_u64_at(std::vector<uint8_t> &bytes, size_t offset, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void
store_digest_at(std::vector<uint8_t> &bytes, size_t offset,
                const ArtifactDigest &digest)
{
  std::ranges::copy(digest, bytes.begin() + static_cast<ptrdiff_t>(offset));
}

void
expect_artifact_error(std::vector<uint8_t> artifact, ArtifactError expected)
{
  const auto parsed = parse_artifact(artifact, Rv32Limits::defaults());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error(), expected);
}

struct HaltHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    ++calls;
    last_args = args;
    last_pc = view.pc();
    return HostResult::halt(77);
  }

  int calls = 0;
  EcallArgs last_args;
  uint32_t last_pc = 0;
};

struct ContinueThenHaltHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)view;
    (void)args;
    ++calls;
    return calls == 1 ? HostResult::continue_with(123) : HostResult::halt();
  }

  int calls = 0;
};

struct MemoryHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)args;
    const std::array<uint8_t, 4> data = {1, 2, 3, 4};
    wrote = view.write_memory(8, data);
    return HostResult::halt();
  }

  bool wrote = false;
};

struct TrapHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)view;
    (void)args;
    ++calls;
    return HostResult::trap(0, 77);
  }

  int calls = 0;
};

struct OutOfGasHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)view;
    (void)args;
    ++calls;
    return HostResult::out_of_gas();
  }

  int calls = 0;
};

struct MutatingOutOfGasHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)args;
    ++calls;
    const std::array<uint8_t, 4> data = {0xaa, 0xbb, 0xcc, 0xdd};
    wrote = view.write_memory(4, data);
    return HostResult::out_of_gas();
  }

  int calls = 0;
  bool wrote = false;
};

struct MutatingTrapHost : Host {
  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
  {
    (void)args;
    ++calls;
    const std::array<uint8_t, 4> data = {0xaa, 0xbb, 0xcc, 0xdd};
    wrote = view.write_memory(4, data);
    return HostResult::trap();
  }

  int calls = 0;
  bool wrote = false;
};

TEST(Rv32MachineYieldApi, startYieldsEcallThenResumeContinues)
{
  // addi a0,x0,1 ; ecall ; addi a1,x0,5  -> after ecall, a0 := continue_with(9)
  const auto program =
      make_program({addi(10, 0, 1), 0x00000073U, addi(11, 0, 5)});
  Machine machine(program, 64, 100);

  EXPECT_EQ(machine.start(), StepResult::kEcall);
  ASSERT_TRUE(machine.pending_ecall().has_value());
  EXPECT_EQ(machine.pending_ecall()->a0, 1U);

  // Continue: write a0=9, run on to fall-off-end trap.
  const StepResult r = machine.resume_from_ecall(HostResult::continue_with(9));
  EXPECT_EQ(r, StepResult::kTrap);         // falls off the end after addi a1
  EXPECT_EQ(machine.trap(), Trap::kBadPc); // fall-off-end is a bad-pc trap
  EXPECT_EQ(machine.reg(10), 9U);          // a0 from continue_with
  EXPECT_EQ(machine.reg(11), 5U);          // execution continued past the ecall
}

TEST(Rv32MachineYieldApi, resumeFromEcallHaltAndTrapAreTerminal)
{
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 16, 100);
    EXPECT_EQ(machine.start(), StepResult::kEcall);
    EXPECT_EQ(machine.resume_from_ecall(HostResult::halt(7)),
              StepResult::kHalted);
    EXPECT_EQ(machine.halt_code(), 7U);
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 16, 100);
    EXPECT_EQ(machine.start(), StepResult::kEcall);
    EXPECT_EQ(machine.resume_from_ecall(HostResult::trap(0, 42)),
              StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kHostTrap);
    EXPECT_EQ(machine.host_trap_code(), 42U);
  }
}

TEST(Rv32MachineYieldApi, startResumeMatchesRunForSameHost)
{
  // Drive one Machine manually via start()/resume_from_ecall and another via
  // run(Host&) with the SAME host program; assert identical final state.
  // MemoryHost writes {1,2,3,4} at offset 8 then halts(0) on its first ecall.
  const auto program = make_program({addi(10, 0, 1), 0x00000073U});

  Machine driven(program, 64, 100);
  MemoryHost manual_host;
  StepResult r = driven.start();
  // Mid-flight state run() hides: start() stops at the first ecall.
  EXPECT_EQ(r, StepResult::kEcall);
  ASSERT_TRUE(driven.pending_ecall().has_value());
  // ecall word is at byte 4; the seam advances pc past it before yielding.
  EXPECT_EQ(driven.pc(), 8U);
  while (r == StepResult::kEcall) {
    r = driven.resume_from_ecall(
        manual_host.ecall(driven.view(), driven.pending_ecall().value()));
  }

  Machine ran(program, 64, 100);
  MemoryHost run_host;
  const StepResult rr = ran.run(run_host);

  EXPECT_EQ(r, rr);
  EXPECT_EQ(r, StepResult::kHalted);
  EXPECT_EQ(driven.pc(), ran.pc());
  EXPECT_EQ(driven.gas_remaining(), ran.gas_remaining());
  EXPECT_EQ(driven.halt_code(), ran.halt_code());
  EXPECT_EQ(driven.halt_code(), 0U); // MemoryHost halts with code 0
  EXPECT_TRUE(manual_host.wrote);
  EXPECT_TRUE(run_host.wrote);
  // MemoryHost writes {1,2,3,4} at offset 8.
  EXPECT_EQ(driven.memory()[8], ran.memory()[8]);
  EXPECT_EQ(driven.memory()[8], 1U);
  EXPECT_EQ(driven.memory()[11], ran.memory()[11]);
  EXPECT_EQ(driven.memory()[11], 4U);
}

std::string
digest_hex(const ArtifactDigest &digest)
{
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  for (const uint8_t byte : digest) {
    out += kHex[byte >> 4U];
    out += kHex[byte & 0x0FU];
  }
  return out;
}

std::vector<uint8_t>
ascii_bytes(std::string_view text)
{
  return std::vector<uint8_t>(text.begin(), text.end());
}

// The section digest algorithm (kArtifactSectionDigestAlgorithm) is
// consensus-visible: every validator must derive the same bytes from the same
// section, or the same artifact is admitted on some nodes and rejected on
// others. Pin artifact_digest() to the published SHA-256 vectors so a swap of
// the underlying digest call that changes those bytes fails here instead of in
// consensus. Regression test for hogd
// issue #2696, where a failing one-shot OpenSSL SHA256() left the digest all
// zeros and so admitted artifacts carrying all-zero section digests.
TEST(Rv32ArtifactTest, computesCanonicalSha256SectionDigests)
{
  EXPECT_EQ(digest_hex(artifact_digest({})),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(digest_hex(artifact_digest(ascii_bytes("abc"))),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(digest_hex(artifact_digest(ascii_bytes(
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Rv32ArtifactTest, parsesCompiledFixtureArtifact)
{
  const auto parsed =
      parse_artifact(compiled_sha256_return::kArtifact, Rv32Limits::defaults());

  ASSERT_TRUE(parsed.has_value());
  ASSERT_NE(parsed->program, nullptr);
  EXPECT_EQ(parsed->program->byte_size(), compiled_sha256_return::kCodeBytes);
  // v5: the stored image is the initialized span [start, start+size);
  // the runtime zero-fills around it.
  EXPECT_EQ(parsed->memory_bytes, compiled_sha256_return::kMemoryBytes);
  EXPECT_EQ(load_u32_le(compiled_sha256_return::kArtifact,
                        kArtifactMemoryBytesOffset),
            compiled_sha256_return::kMemoryBytes);
  EXPECT_EQ(load_u32_le(compiled_sha256_return::kArtifact,
                        kArtifactInitialMemoryStartOffset),
            compiled_sha256_return::kInitialMemoryStart);
  EXPECT_GT(compiled_sha256_return::kInitialMemoryStart, 0U);
  ASSERT_EQ(parsed->initial_memory.size(),
            compiled_sha256_return::kInitialMemoryStart +
                compiled_sha256_return::kInitialMemoryBytes);
  EXPECT_LE(compiled_sha256_return::kInitialMemoryStart +
                compiled_sha256_return::kInitialMemoryBytes,
            compiled_sha256_return::kMemoryBytes);
  EXPECT_TRUE(std::ranges::equal(
      std::span<const uint8_t>(compiled_sha256_return::kInput),
      std::span<const uint8_t>(parsed->initial_memory)
          .subspan(compiled_sha256_return::kFixtureInputOffset,
                   compiled_sha256_return::kInput.size())));
}

// Replace a v6 artifact's (empty) extension list with `words`, big-endian.
std::vector<uint8_t>
with_extension_words(std::vector<uint8_t> artifact,
                     std::initializer_list<uint32_t> words)
{
  artifact.resize(artifact.size() - 4);
  for (uint32_t word : words) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      artifact.push_back(static_cast<uint8_t>(word >> shift));
    }
  }
  return artifact;
}

TEST(Rv32ArtifactTest, extensionsDoNotEnterGuestMemory)
{
  auto artifact = valid_artifact_bytes();
  auto baseline = parse_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(baseline.has_value());
  // Kinds 1 and 99, the first with a 5-byte body and three zero pad bytes.
  artifact = with_extension_words(std::move(artifact),
                                  {2, 1, 5, 0x01020304, 0x05000000, 99, 0});
  auto parsed = parse_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->initial_memory, baseline->initial_memory);
  EXPECT_EQ(parsed->memory_bytes, baseline->memory_bytes);
  EXPECT_EQ(parsed->program->byte_size(), baseline->program->byte_size());
}

TEST(Rv32ArtifactTest, legacyLayoutMatchesCurrentImage)
{
  auto artifact = valid_artifact_bytes();
  auto baseline = parse_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(baseline.has_value());
  // v5 has the same header and ends after initial memory: no extension list.
  artifact.resize(artifact.size() - 4);
  store_u32_at(artifact, kArtifactFormatVersionOffset, kArtifactVersion5);
  auto parsed = parse_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->initial_memory, baseline->initial_memory);
  EXPECT_EQ(parsed->memory_bytes, baseline->memory_bytes);
  EXPECT_EQ(parsed->program->byte_size(), baseline->program->byte_size());
  auto threaded = parse_threaded_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(threaded.has_value());
  EXPECT_EQ(threaded->code.data(), artifact.data() + kArtifactHeaderBytes);

  auto corrupt = artifact;
  corrupt[kArtifactHeaderBytes] ^= 1;
  expect_artifact_error(std::move(corrupt), ArtifactError::kCodeDigestMismatch);
  auto trailing = artifact;
  trailing.push_back(0);
  expect_artifact_error(std::move(trailing),
                        ArtifactError::kInvalidSectionLayout);
  // Even an empty extension list is trailing data in v5.
  auto with_list = artifact;
  with_list.insert(with_list.end(), {0, 0, 0, 0});
  expect_artifact_error(std::move(with_list),
                        ArtifactError::kInvalidSectionLayout);
  auto wrong_header = artifact;
  store_u32_at(wrong_header, kArtifactHeaderSizeOffset,
               kArtifactHeaderBytes + 8);
  expect_artifact_error(std::move(wrong_header), ArtifactError::kBadHeaderSize);
  artifact.resize(kArtifactHeaderBytes - 1);
  expect_artifact_error(std::move(artifact), ArtifactError::kTooSmall);
}

TEST(Rv32ArtifactTest, rejectsNonCanonicalExtensionList)
{
  const std::initializer_list<uint32_t> cases[] = {
      {},                    // missing list
      {1, 0, 0},             // kind 0
      {2, 5, 0, 5, 0},       // duplicate kind
      {2, 5, 0, 4, 0},       // descending kinds
      {1, 7, 1, 0x01000001}, // nonzero padding
      {1, 7, 0, 0},          // trailing bytes
      {2, 7, 0},             // count past the end
      {1, 7, 8, 0},          // body past the end
      {1, 7, UINT32_MAX},    // padded size past the end
  };
  for (const auto &words : cases) {
    expect_artifact_error(with_extension_words(valid_artifact_bytes(), words),
                          ArtifactError::kInvalidSectionLayout);
  }
}

TEST(Rv32ArtifactTest, rejectsHeaderVersionAndProfileMismatches)
{
  {
    auto artifact = valid_artifact_bytes();
    artifact[0] ^= 0xffU;
    expect_artifact_error(std::move(artifact), ArtifactError::kBadMagic);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactFormatVersionOffset, kArtifactVersion6 + 1);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kUnsupportedFormatVersion);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactHeaderSizeOffset, kArtifactHeaderBytes + 4);
    expect_artifact_error(std::move(artifact), ArtifactError::kBadHeaderSize);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u64_at(artifact, kArtifactVmProfileOffset, 0);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kUnsupportedVmProfile);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactFlagsOffset, 1);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kUnsupportedFlags);
  }
}

TEST(Rv32ArtifactTest, rejectsAbiAndGasMismatches)
{
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactEntryAbiOffset,
                 kArtifactEntryAbiVersion + 1);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kUnsupportedEntryAbi);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactSyscallAbiOffset,
                 kArtifactSyscallAbiVersion + 1);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kUnsupportedSyscallAbi);
  }
  {
    // The pre-register-native syscall ABI (version 0) must be rejected, not
    // just unknown future versions: loading a v0 (memory-frame) artifact under
    // the v1 register-native bridge would silently mis-marshal syscall
    // arguments.
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactSyscallAbiOffset, 0);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kUnsupportedSyscallAbi);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactGasVersionOffset, kGasTableVersion + 1);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kGasScheduleVersionMismatch);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u64_at(artifact, kArtifactGasDigestOffset, kGasScheduleDigest ^ 1ULL);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kGasScheduleDigestMismatch);
  }
}

TEST(Rv32ArtifactTest, acceptsOnlyRegisteredGasSchedulePairs)
{
  // Gas version and digest are one identity; reject cross-paired schedules.
  auto transitional = valid_artifact_bytes();
  store_u32_at(transitional, kArtifactGasVersionOffset,
               kTransitionalGasTableVersion);
  store_u64_at(transitional, kArtifactGasDigestOffset,
               kTransitionalGasScheduleDigest);
  EXPECT_TRUE(parse_artifact(transitional, Rv32Limits::defaults()).has_value());
  EXPECT_TRUE(parse_threaded_artifact(transitional, Rv32Limits::defaults())
                  .has_value());

  auto current_version_transitional_digest = valid_artifact_bytes();
  store_u64_at(current_version_transitional_digest, kArtifactGasDigestOffset,
               kTransitionalGasScheduleDigest);
  expect_artifact_error(std::move(current_version_transitional_digest),
                        ArtifactError::kGasScheduleDigestMismatch);

  auto transitional_version_current_digest = valid_artifact_bytes();
  store_u32_at(transitional_version_current_digest, kArtifactGasVersionOffset,
               kTransitionalGasTableVersion);
  expect_artifact_error(std::move(transitional_version_current_digest),
                        ArtifactError::kGasScheduleDigestMismatch);
}

TEST(Rv32ArtifactTest, rejectsExtremeHeaderIntegersWithoutWrapping)
{
  // Reject extreme sizes before forming section ranges or allocating memory.
  struct ExtremeFieldCase {
    size_t offset;
    ArtifactError error;
  };
  constexpr std::array<ExtremeFieldCase, 4> kCases = {{
      {kArtifactMemoryBytesOffset, ArtifactError::kMemorySizeInvalid},
      {kArtifactCodeSizeOffset, ArtifactError::kCodeSizeInvalid},
      {kArtifactInitialMemorySizeOffset, ArtifactError::kInitialMemoryTooLarge},
      {kArtifactInitialMemoryStartOffset,
       ArtifactError::kInitialMemoryTooLarge},
  }};
  for (const auto &[offset, error] : kCases) {
    SCOPED_TRACE(offset);
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, offset, std::numeric_limits<uint32_t>::max());
    const auto canonical = parse_artifact(artifact, Rv32Limits::defaults());
    const auto threaded =
        parse_threaded_artifact(artifact, Rv32Limits::defaults());
    ASSERT_FALSE(canonical.has_value());
    ASSERT_FALSE(threaded.has_value());
    EXPECT_EQ(canonical.error(), error);
    EXPECT_EQ(threaded.error(), error);
  }

  // With permissive caller limits, start+size would wrap in uint32_t. The
  // subtraction-form placement check must still reject the image as too large.
  auto wrapping_placement = valid_artifact_bytes();
  store_u32_at(wrapping_placement, kArtifactMemoryBytesOffset,
               std::numeric_limits<uint32_t>::max());
  store_u32_at(wrapping_placement, kArtifactInitialMemoryStartOffset,
               std::numeric_limits<uint32_t>::max() - 1);
  constexpr Rv32Limits kPermissive = {
      .max_code_bytes = std::numeric_limits<uint32_t>::max(),
      .max_memory_bytes = std::numeric_limits<uint32_t>::max(),
  };
  const auto canonical = parse_artifact(wrapping_placement, kPermissive);
  const auto threaded =
      parse_threaded_artifact(wrapping_placement, kPermissive);
  ASSERT_FALSE(canonical.has_value());
  ASSERT_FALSE(threaded.has_value());
  EXPECT_EQ(canonical.error(), ArtifactError::kInitialMemoryTooLarge);
  EXPECT_EQ(threaded.error(), ArtifactError::kInitialMemoryTooLarge);
}

TEST(Rv32ArtifactTest, honorsZeroAndExactCallerLimits)
{
  // Check zero, one-below, and exact caller limits without rounding.
  const auto artifact = valid_artifact_bytes();
  const uint32_t code_size = load_u32_le(artifact, kArtifactCodeSizeOffset);
  const uint32_t memory_size =
      load_u32_le(artifact, kArtifactMemoryBytesOffset);

  auto expect_both = [&](Rv32Limits limits, bool accepted,
                         ArtifactError rejected_as) {
    const auto canonical = parse_artifact(artifact, limits);
    const auto threaded = parse_threaded_artifact(artifact, limits);
    EXPECT_EQ(canonical.has_value(), accepted);
    EXPECT_EQ(threaded.has_value(), accepted);
    if (!accepted) {
      ASSERT_FALSE(canonical.has_value());
      ASSERT_FALSE(threaded.has_value());
      EXPECT_EQ(canonical.error(), rejected_as);
      EXPECT_EQ(threaded.error(), rejected_as);
    }
  };

  expect_both({.max_code_bytes = 0, .max_memory_bytes = memory_size}, false,
              ArtifactError::kCodeSizeInvalid);
  expect_both({.max_code_bytes = code_size - 1,
               .max_memory_bytes = memory_size},
              false, ArtifactError::kCodeSizeInvalid);
  expect_both({.max_code_bytes = code_size,
               .max_memory_bytes = memory_size - 1},
              false, ArtifactError::kMemorySizeInvalid);
  expect_both({.max_code_bytes = code_size, .max_memory_bytes = memory_size},
              true, ArtifactError::kTooSmall);

  const auto zero_memory =
      test_artifact::build_artifact({addi(0, 0, 0)},
                                    std::span<const uint8_t>{});
  const Rv32Limits zero_memory_limit = {
      .max_code_bytes = kInstructionBytes,
      .max_memory_bytes = 0,
  };
  EXPECT_TRUE(parse_artifact(zero_memory, zero_memory_limit).has_value());
  EXPECT_TRUE(
      parse_threaded_artifact(zero_memory, zero_memory_limit).has_value());
}

// The admitted format versions are the caller's too: a format this VM reads
// is refused outside the range, on both parse paths, and the ceiling limits a
// deployed contract is instantiated under read every format.
TEST(Rv32ArtifactTest, honorsCallerArtifactVersionRange)
{
  const auto current = valid_artifact_bytes();
  auto legacy = valid_artifact_bytes();
  store_u32_at(legacy, kArtifactFormatVersionOffset, kArtifactVersion5);

  auto admitted = [](std::vector<uint8_t> const &artifact, uint32_t min,
                     uint32_t max) {
    Rv32Limits limits = Rv32Limits::defaults();
    limits.min_artifact_version = min;
    limits.max_artifact_version = max;
    const auto canonical = parse_artifact(artifact, limits);
    const auto threaded = parse_threaded_artifact(artifact, limits);
    EXPECT_EQ(canonical.has_value(), threaded.has_value());
    if (!canonical.has_value()) {
      EXPECT_EQ(canonical.error(), ArtifactError::kUnsupportedFormatVersion);
    }
    if (!threaded.has_value()) {
      EXPECT_EQ(threaded.error(), ArtifactError::kUnsupportedFormatVersion);
    }
    return canonical.has_value();
  };

  EXPECT_TRUE(admitted(legacy, kArtifactVersion5, kArtifactVersion6));
  EXPECT_TRUE(admitted(current, kArtifactVersion5, kArtifactVersion6));

  // Raising the minimum retires the old format from attachment.
  EXPECT_FALSE(admitted(legacy, kArtifactVersion6, kArtifactVersion6));
  EXPECT_TRUE(admitted(current, kArtifactVersion6, kArtifactVersion6));

  // Lowering the maximum holds the new one back.
  EXPECT_TRUE(admitted(legacy, kArtifactVersion5, kArtifactVersion5));
  EXPECT_FALSE(admitted(current, kArtifactVersion5, kArtifactVersion5));

  // An inverted range admits nothing, and a wide one adds no format the VM
  // does not read.
  EXPECT_FALSE(admitted(legacy, kArtifactVersion6, kArtifactVersion5));
  EXPECT_FALSE(admitted(current, kArtifactVersion6, kArtifactVersion5));
  auto future = valid_artifact_bytes();
  store_u32_at(future, kArtifactFormatVersionOffset, kArtifactVersion6 + 1);
  EXPECT_FALSE(admitted(future, 0, std::numeric_limits<uint32_t>::max()));

  EXPECT_TRUE(parse_artifact(legacy, Rv32Limits::ceiling()).has_value());
  EXPECT_TRUE(parse_artifact(current, Rv32Limits::ceiling()).has_value());
}

TEST(Rv32ArtifactTest, defersMisalignedCodeOnlyOnThreadedParsePath)
{
  // The threaded parser defers alignment checks to pre-instantiation.
  for (const uint32_t malformed_code_size : {1U, 2U, 3U}) {
    SCOPED_TRACE(malformed_code_size);
    auto artifact = valid_artifact_bytes();
    const uint32_t original_code_size =
        load_u32_le(artifact, kArtifactCodeSizeOffset);
    artifact.erase(artifact.begin() + kArtifactHeaderBytes +
                       malformed_code_size,
                   artifact.begin() + kArtifactHeaderBytes +
                       original_code_size);
    store_u32_at(artifact, kArtifactCodeSizeOffset, malformed_code_size);
    const auto malformed_code =
        std::span<const uint8_t>(artifact).subspan(kArtifactHeaderBytes,
                                                   malformed_code_size);
    store_digest_at(artifact, kArtifactCodeDigestOffset,
                    artifact_digest(malformed_code));

    const auto canonical = parse_artifact(artifact, Rv32Limits::defaults());
    ASSERT_FALSE(canonical.has_value());
    EXPECT_EQ(canonical.error(), ArtifactError::kInvalidProgram);

    const auto threaded =
        parse_threaded_artifact(artifact, Rv32Limits::defaults());
    ASSERT_TRUE(threaded.has_value());
    const auto pre =
        ThreadedMachinePreInstantiation::create(threaded->code,
                                                threaded->initial_memory,
                                                threaded->initial_memory_start,
                                                threaded->memory_bytes,
                                                /*trace=*/false);
    ASSERT_FALSE(pre.has_value());
    EXPECT_EQ(pre.error(), ProgramError::kCodeSizeNotAligned);
  }
}

// The code and memory bounds are the caller's, not the format's: an artifact
// past the protocol defaults is refused under them and admitted under a
// chain's raised limits, and only the header's declaration is checked -- the
// body still has to be what it says.
TEST(Rv32ArtifactTest, boundsComeFromTheCallersLimits)
{
  constexpr Rv32Limits kRaised = {.max_code_bytes = 2 * kDefaultMaxCodeBytes,
                                  .max_memory_bytes =
                                      2 * kDefaultMaxMemoryBytes};

  // One instruction past the default code size.
  const std::vector<uint32_t> words((kDefaultMaxCodeBytes / kInstructionBytes) +
                                        1,
                                    addi(0, 0, 0));
  auto big_code =
      test_artifact::build_artifact(std::span<const uint32_t>(words.data(),
                                                              words.size()),
                                    std::span<const uint8_t>{});
  {
    const auto parsed = parse_artifact(big_code, Rv32Limits::defaults());
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), ArtifactError::kCodeSizeInvalid);
    EXPECT_FALSE(
        parse_threaded_artifact(big_code, Rv32Limits::defaults()).has_value());
  }
  {
    const auto parsed = parse_artifact(big_code, kRaised);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->program->instruction_count(), words.size());
    EXPECT_TRUE(parse_threaded_artifact(big_code, kRaised).has_value());
  }

  // One byte past the default memory size, declared in the header alone: the
  // stored image stays small and the runtime zero-extends.
  auto big_memory = valid_artifact_bytes();
  store_u32_at(big_memory, kArtifactMemoryBytesOffset,
               kDefaultMaxMemoryBytes + 1);
  {
    const auto parsed = parse_artifact(big_memory, Rv32Limits::defaults());
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), ArtifactError::kMemorySizeInvalid);
  }
  {
    const auto parsed = parse_artifact(big_memory, kRaised);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->memory_bytes, kDefaultMaxMemoryBytes + 1);
    const auto threaded = parse_threaded_artifact(big_memory, kRaised);
    ASSERT_TRUE(threaded.has_value());
    EXPECT_EQ(threaded->memory_bytes, kDefaultMaxMemoryBytes + 1);
  }

  // The VM's ceilings are the most any caller may pass and still be honoured.
  auto at_ceiling = valid_artifact_bytes();
  store_u32_at(at_ceiling, kArtifactMemoryBytesOffset, kMemoryCeilingBytes);
  EXPECT_TRUE(parse_artifact(at_ceiling, Rv32Limits::ceiling()).has_value());
}

TEST(Rv32ArtifactTest, rejectsBoundsAndSectionLayoutErrors)
{
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactMemoryBytesOffset,
                 kDefaultMaxMemoryBytes + 1);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kMemorySizeInvalid);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactCodeSizeOffset, 0);
    expect_artifact_error(std::move(artifact), ArtifactError::kCodeSizeInvalid);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactCodeSizeOffset, kDefaultMaxCodeBytes + 1);
    expect_artifact_error(std::move(artifact), ArtifactError::kCodeSizeInvalid);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactInitialMemorySizeOffset,
                 kDefaultMaxMemoryBytes + 1);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kInitialMemoryTooLarge);
  }
  {
    // initial_memory_start pushes the image past memory_bytes.
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactInitialMemoryStartOffset, 1);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kInitialMemoryTooLarge);
  }
  {
    // Canonicality: an empty image must have start == 0.
    auto artifact = test_artifact::build_artifact({addi(0, 0, 0), 0x00000073U},
                                                  std::span<const uint8_t>{});
    store_u32_at(artifact, kArtifactMemoryBytesOffset, 64);
    store_u32_at(artifact, kArtifactInitialMemoryStartOffset, 4);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kInitialMemoryTooLarge);
  }
  {
    // Trailing byte breaks the exact header||code||initial_memory layout.
    auto artifact = valid_artifact_bytes();
    artifact.push_back(0);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kInvalidSectionLayout);
  }
  {
    // Truncated body.
    auto artifact = valid_artifact_bytes();
    artifact.pop_back();
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kInvalidSectionLayout);
  }
  {
    // code_size larger than the body actually carries.
    auto artifact = valid_artifact_bytes();
    const uint32_t code_size = load_u32_le(artifact, kArtifactCodeSizeOffset);
    store_u32_at(artifact, kArtifactCodeSizeOffset, code_size + 4);
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kInvalidSectionLayout);
  }
}

TEST(Rv32ArtifactTest, rejectsDigestAndProgramErrors)
{
  {
    auto artifact = valid_artifact_bytes();
    store_digest_at(artifact, kArtifactCodeDigestOffset, ArtifactDigest{});
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kCodeDigestMismatch);
  }
  {
    auto artifact = valid_artifact_bytes();
    store_digest_at(artifact, kArtifactInitialMemoryDigestOffset,
                    ArtifactDigest{});
    expect_artifact_error(std::move(artifact),
                          ArtifactError::kInitialMemoryDigestMismatch);
  }
  {
    auto artifact = valid_artifact_bytes();
    const uint32_t code_size = load_u32_le(artifact, kArtifactCodeSizeOffset);
    artifact[kArtifactHeaderBytes] = 0;
    const auto code =
        std::span<const uint8_t>(artifact).subspan(kArtifactHeaderBytes,
                                                   code_size);
    store_digest_at(artifact, kArtifactCodeDigestOffset, artifact_digest(code));
    expect_artifact_error(std::move(artifact), ArtifactError::kInvalidProgram);
  }
  {
    auto artifact = valid_artifact_bytes();
    artifact.resize(kArtifactHeaderBytes - 1);
    expect_artifact_error(std::move(artifact), ArtifactError::kTooSmall);
  }
}

TEST(Rv32ArtifactTest, acceptsZeroExtendedInitialMemoryPrefix)
{
  // initial_memory_size may be < memory_bytes; the runtime zero-extends the
  // tail. build_artifact stamps memory_bytes == the stored prefix, so bump
  // the (non-digest-covered) memory_bytes field to a larger address space
  // and confirm the shorter stored prefix still parses.
  const std::array<uint8_t, 4> stored = {1, 2, 3, 4};
  auto artifact =
      test_artifact::build_artifact({addi(0, 0, 0), 0x00000073U}, stored);
  store_u32_at(artifact, kArtifactMemoryBytesOffset, 256);
  const auto parsed = parse_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->memory_bytes, 256U);
  ASSERT_EQ(parsed->initial_memory.size(), stored.size());
  EXPECT_TRUE(std::ranges::equal(parsed->initial_memory, stored));
}

TEST(Rv32ArtifactTest, acceptsZeroMemoryBytesArtifact)
{
  // The canonical zero-memory encoding: no stored image, memory_bytes == 0.
  // A zero-memory contract is a valid degenerate (all data accesses fault).
  const auto artifact =
      test_artifact::build_artifact({addi(1, 0, 7), 0x00100073U},
                                    std::span<const uint8_t>{});
  const auto parsed = parse_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->memory_bytes, 0U);
  EXPECT_TRUE(parsed->initial_memory.empty());

  const auto threaded =
      parse_threaded_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(threaded.has_value());
  EXPECT_EQ(threaded->memory_bytes, 0U);
  EXPECT_TRUE(threaded->initial_memory.empty());
}

TEST(Rv32ArtifactTest, acceptsLimitBoundaryValues)
{
  // Only the reject side of each limit was pinned; an off-by-one tightening
  // would reject artifacts other validators accept.
  {
    auto artifact = valid_artifact_bytes();
    store_u32_at(artifact, kArtifactMemoryBytesOffset, kDefaultMaxMemoryBytes);
    const auto parsed = parse_artifact(artifact, Rv32Limits::defaults());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->memory_bytes, kDefaultMaxMemoryBytes);
    EXPECT_TRUE(
        parse_threaded_artifact(artifact, Rv32Limits::defaults()).has_value());
  }
  {
    const std::vector<uint32_t> words((kDefaultMaxCodeBytes /
                                       kInstructionBytes),
                                      addi(0, 0, 0));
    const std::array<uint8_t, 1> stored = {1};
    const auto artifact = test_artifact::build_artifact(words, stored);
    const auto parsed = parse_artifact(artifact, Rv32Limits::defaults());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(
        parse_threaded_artifact(artifact, Rv32Limits::defaults()).has_value());
  }
}

TEST(Rv32ProgramTest, zero_memory_machine_faults_all_data_accesses)
{
  const auto program = make_program({load(1, 2, 0, 0)}); // lw x1, 0(x0)
  Machine machine(program, /*memory_bytes=*/0, /*gas=*/100);
  EXPECT_EQ(machine.trap(), Trap::kNone);
  EXPECT_EQ(machine.step(), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadMemory);
}

TEST(Rv32ProgramTest, construction_traps_bad_pc_for_invalid_initial_pc)
{
  const auto program = make_program({addi(1, 0, 7)});

  Machine out_of_range(program, 31, 5, /*initial_pc=*/8);
  EXPECT_EQ(out_of_range.trap(), Trap::kBadPc);
  EXPECT_EQ(out_of_range.step(), StepResult::kTrap);
  EXPECT_EQ(out_of_range.gas_remaining(),
            5U); // never executed, nothing charged
  out_of_range.reset(9);
  EXPECT_EQ(out_of_range.trap(), Trap::kBadPc);
  EXPECT_EQ(out_of_range.gas_remaining(), 9U);

  Machine misaligned(program, 31, 5, /*initial_pc=*/2);
  EXPECT_EQ(misaligned.trap(), Trap::kBadPc);
  EXPECT_EQ(misaligned.step(), StepResult::kTrap);
  EXPECT_EQ(misaligned.gas_remaining(), 5U);
}

TEST(Rv32ArtifactTest, materializesInitialMemoryAtStartOffset)
{
  const std::array<uint8_t, 4> stored = {1, 2, 3, 4};
  auto artifact =
      test_artifact::build_artifact({addi(0, 0, 0), 0x00000073U}, stored,
                                    /*initial_memory_start=*/8);

  const auto parsed = parse_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(parsed.has_value());
  // build_artifact stamps memory_bytes = start + size.
  EXPECT_EQ(parsed->memory_bytes, 12U);
  // The copying path materializes the zero prefix: [0, start + size).
  ASSERT_EQ(parsed->initial_memory.size(), 12U);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(parsed->initial_memory[i], 0U) << i;
  }
  EXPECT_TRUE(std::ranges::equal(
      std::span<const uint8_t>(parsed->initial_memory).subspan(8), stored));

  // The zero-copy path exposes the offset instead of materializing.
  const auto threaded =
      parse_threaded_artifact(artifact, Rv32Limits::defaults());
  ASSERT_TRUE(threaded.has_value());
  EXPECT_EQ(threaded->initial_memory_start, 8U);
  EXPECT_EQ(threaded->memory_bytes, 12U);
  EXPECT_TRUE(std::ranges::equal(threaded->initial_memory, stored));
}

TEST(Rv32ThreadedHostileInput, translatesCodeAtExactVmCeiling)
{
  // Each four-byte guest instruction expands into a 16-byte host slot.
  std::vector<uint8_t> code;
  code.reserve(kCodeCeilingBytes);
  while (code.size() < kCodeCeilingBytes) {
    append_u32(code, addi(0, 0, 0));
  }
  ASSERT_EQ(code.size(), kCodeCeilingBytes);

  const auto pre =
      ThreadedMachinePreInstantiation::create(code, /*memory=*/{},
                                              /*memory_start=*/0,
                                              /*total_memory_size=*/0,
                                              /*trace=*/false);
  EXPECT_TRUE(pre.has_value());
}

TEST(Rv32ThreadedHostileInput,
     retainsMultipleRaisedLimitTranslationsIndependently)
{
  // Retain several translations to catch aliasing and lifetime bugs, using
  // default-sized code and above-default memory to limit CI memory use
  // compared with ceiling programs.
  constexpr size_t kRetainedPrograms = 4;
  constexpr uint32_t kLargeCodeBytes = 256 * 1024;
  constexpr uint32_t kLargeMemoryBytes = 256 * 1024;
  std::vector<ThreadedMachinePreInstantiation> retained;
  retained.reserve(kRetainedPrograms);

  for (size_t i = 0; i < kRetainedPrograms; ++i) {
    std::vector<uint8_t> code;
    code.reserve(kLargeCodeBytes);
    append_u32(code, addi(1, 0, static_cast<int32_t>(i + 1)));
    append_u32(code, 0x00100073U); // ebreak terminates the short execution.
    while (code.size() < kLargeCodeBytes) {
      append_u32(code, addi(0, 0, 0));
    }
    std::vector<uint8_t> memory(kLargeMemoryBytes, static_cast<uint8_t>(i + 1));
    auto pre = ThreadedMachinePreInstantiation::create(code, memory,
                                                       /*memory_start=*/0,
                                                       kLargeMemoryBytes,
                                                       /*trace=*/false);
    ASSERT_TRUE(pre.has_value());
    retained.emplace_back(std::move(*pre));
  }

  for (size_t i = 0; i < retained.size(); ++i) {
    ThreadedMachine machine(retained[i], /*gas=*/10);
    EXPECT_EQ(machine.memory().front(), static_cast<uint8_t>(i + 1));
    EXPECT_EQ(machine.start(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBreakpoint);
    EXPECT_EQ(machine.reg(1), i + 1);
  }
}

TEST(Rv32ThreadedHostileInput, boundsMemoryPoolAtIdleSlotCapacity)
{
  // Exceed the idle-pool capacity, then check bounded reuse of clean buffers.
  memory_slot_pool() = MemorySlotPool{};
  const auto code = code_from_words({addi(0, 0, 0)});
  auto pre = ThreadedMachinePreInstantiation::create(code, /*memory=*/{},
                                                     /*memory_start=*/0,
                                                     /*total_memory_size=*/64,
                                                     /*trace=*/false);
  ASSERT_TRUE(pre.has_value());

  {
    std::vector<ThreadedMachine> live;
    live.reserve(kMemorySlotPoolCap + 1);
    std::vector<const uint8_t *> buffers;
    buffers.reserve(kMemorySlotPoolCap + 1);
    for (size_t i = 0; i <= kMemorySlotPoolCap; ++i) {
      live.emplace_back(*pre, /*gas=*/10);
      const uint8_t *buffer = live.back().memory().data();
      EXPECT_EQ(std::ranges::count(buffers, buffer), 0U);
      buffers.push_back(buffer);
      live.back().memory().front() = static_cast<uint8_t>(i + 1);
    }
    EXPECT_EQ(memory_slot_pool().count, 0U);
  }
  EXPECT_EQ(memory_slot_pool().count, kMemorySlotPoolCap);

  {
    std::vector<ThreadedMachine> reused;
    reused.reserve(kMemorySlotPoolCap);
    for (size_t i = 0; i < kMemorySlotPoolCap; ++i) {
      reused.emplace_back(*pre, /*gas=*/10);
      EXPECT_TRUE(std::ranges::all_of(reused.back().memory(),
                                      [](uint8_t byte) { return byte == 0; }));
    }
    EXPECT_EQ(memory_slot_pool().count, 0U);
  }
  EXPECT_EQ(memory_slot_pool().count, kMemorySlotPoolCap);
  memory_slot_pool() = MemorySlotPool{};
}

TEST(Rv32ThreadedHostileInput, resetRestoresAllMutableExecutionState)
{
  // Reset restores the image and clears state after ECALL, trap, or halt.
  const auto code =
      code_from_words({addi(1, 0, 7), 0x00000073U, addi(3, 0, 9)});
  constexpr std::array<uint8_t, 4> kStored = {1, 2, 3, 4};
  auto pre =
      ThreadedMachinePreInstantiation::create(code, kStored, /*memory_start=*/4,
                                              /*total_memory_size=*/16,
                                              /*trace=*/false);
  ASSERT_TRUE(pre.has_value());
  ThreadedMachine machine(*pre, /*gas=*/100);

  machine.memory().front() = 0xaa;
  ASSERT_EQ(machine.start(), StepResult::kEcall);
  ASSERT_TRUE(machine.pending_ecall().has_value());
  EXPECT_EQ(machine.reg(1), 7U);
  machine.reset(/*gas=*/55);
  EXPECT_EQ(machine.pc(), 0U);
  EXPECT_EQ(machine.reg(1), 0U);
  EXPECT_EQ(machine.gas_remaining(), 55U);
  EXPECT_EQ(machine.trap(), Trap::kNone);
  EXPECT_FALSE(machine.halted());
  EXPECT_FALSE(machine.pending_ecall().has_value());
  EXPECT_EQ(machine.memory().front(), 0U);
  EXPECT_TRUE(
      std::ranges::equal(machine.memory().subspan(4, kStored.size()), kStored));

  ASSERT_EQ(machine.start(), StepResult::kEcall);
  EXPECT_EQ(machine.resume_from_ecall(HostResult::continue_with()),
            StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
  machine.reset(/*gas=*/44);
  EXPECT_EQ(machine.trap(), Trap::kNone);
  EXPECT_FALSE(machine.halted());

  ASSERT_EQ(machine.start(), StepResult::kEcall);
  EXPECT_EQ(machine.resume_from_ecall(HostResult::halt(9)),
            StepResult::kHalted);
  EXPECT_TRUE(machine.halted());
  EXPECT_EQ(machine.halt_code(), 9U);
  machine.reset(/*gas=*/33);
  EXPECT_FALSE(machine.halted());
  EXPECT_EQ(machine.halt_code(), 0U);
  EXPECT_EQ(machine.gas_remaining(), 33U);
  EXPECT_EQ(machine.start(), StepResult::kEcall);
}

TEST(Rv32ProgramTest, smoke_builds_standalone_vm)
{
  const auto program = make_program({addi(0, 0, 0)});
  Machine machine(program, 4096, 10);
  EXPECT_EQ(machine.pc(), 0U);
  EXPECT_EQ(machine.reg(0), 0U);
  EXPECT_EQ(machine.reg(2), 0U);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
}

TEST(Rv32MachineSeed, seedsMethodIdIntoA0A1)
{
  const auto program = make_program({addi(0, 0, 0)});
  Machine machine(program, 4096, 10);
  machine.seed_method_id(0xCA571C66A719DD56ULL);
  EXPECT_EQ(machine.reg(10), 0xA719DD56U); // a0 = low 32 bits
  EXPECT_EQ(machine.reg(11), 0xCA571C66U); // a1 = high 32 bits
}

TEST(Rv32ProgramTest, reset_restores_initial_machine_state)
{
  const auto program = make_program({addi(1, 0, 7)});
  Machine machine(program, 31, 5);
  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(1), 7U);

  machine.reset(9);
  EXPECT_EQ(machine.pc(), 0U);
  EXPECT_EQ(machine.reg(1), 0U);
  EXPECT_EQ(machine.reg(2), 0U);
  EXPECT_EQ(machine.gas_remaining(), 9U);
  EXPECT_EQ(machine.trap(), Trap::kNone);

  Machine bad_memory(program, kMemoryCeilingBytes + 1, 5);
  EXPECT_EQ(bad_memory.trap(), Trap::kBadMemory);
  bad_memory.reset(9);
  EXPECT_EQ(bad_memory.trap(), Trap::kBadMemory);
  EXPECT_EQ(bad_memory.step(), StepResult::kTrap);

  Machine bad_program(nullptr, 16, 5);
  EXPECT_EQ(bad_program.trap(), Trap::kInvalidInstruction);
  bad_program.reset(9);
  EXPECT_EQ(bad_program.trap(), Trap::kInvalidInstruction);
  EXPECT_EQ(bad_program.step(), StepResult::kTrap);
}

TEST(Rv32ProgramTest, validates_code_shape_and_profile)
{
  {
    const std::array<uint8_t, 1> code = {0};
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kCodeSizeNotAligned);
  }
  {
    std::vector<uint8_t> code(kCodeCeilingBytes + kInstructionBytes);
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kCodeTooLarge);
  }
  {
    const auto code = code_from_words({0x00000001U});
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedCompressedInstruction);
  }
  {
    const auto code = code_from_words({0x10500073U}); // WFI/system.
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedSystemInstruction);
  }
  {
    const auto code = code_from_words({j_type(0, 8), 0x00100073U, 0x00000073U});
    const auto result = Program::validate(code);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kNone);
  }
  {
    const auto code = code_from_words({0x0000100fU});
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedInstruction);
  }
  {
    const auto code = code_from_words({0x0000007fU});
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedInstruction);
  }
}

TEST(Rv32ProgramTest, validates_profile_boundaries)
{
  {
    std::vector<uint8_t> code;
    code.reserve(kDefaultMaxCodeBytes);
    for (uint32_t i = 0; i < kDefaultMaxCodeBytes / kInstructionBytes; ++i) {
      append_u32(code, addi(0, 0, 0));
    }
    const auto result = Program::validate(code);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kNone);
  }
  {
    const auto code = code_from_words({0x00001073U}); // CSR/system.
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedSystemInstruction);
  }
  {
    const auto code = code_from_words({0x30200073U}); // MRET/privileged.
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedSystemInstruction);
  }
  {
    const auto code = code_from_words({0x0000202fU}); // Atomic opcode.
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedInstruction);
  }
  {
    const auto code = code_from_words({0x00000053U}); // Floating-point op.
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedInstruction);
  }
  {
    const auto code = code_from_words({0x00000007U}); // Floating-point load.
    const auto result = Program::validate(code);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ProgramError::kUnsupportedInstruction);
  }
}

TEST(Rv32ProgramTest, create_returns_validated_immutable_program)
{
  static_assert(!std::is_default_constructible_v<Program>);

  const auto valid = code_from_words({addi(1, 0, 7)});
  auto program = Program::create(valid);
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(program->instruction_count(), 1U);
  EXPECT_EQ(program->byte_size(), kInstructionBytes);

  const std::array<uint8_t, 1> invalid = {0};
  auto error = Program::create(invalid);
  ASSERT_FALSE(error.has_value());
  EXPECT_EQ(error.error(), ProgramError::kCodeSizeNotAligned);
}

TEST(Rv32HostViewTest, checks_memory_ranges_with_machine_invariants)
{
  EXPECT_TRUE(memory_range_in_bounds(16, 12, 4));
  EXPECT_TRUE(memory_range_in_bounds(16, 16, 0));
  EXPECT_FALSE(memory_range_in_bounds(16, 17, 0));
  EXPECT_FALSE(
      memory_range_in_bounds(16, 1, std::numeric_limits<size_t>::max()));
  EXPECT_FALSE(memory_range_in_bounds(16, std::numeric_limits<uint32_t>::max(),
                                      std::numeric_limits<size_t>::max()));

  const auto program = make_program({addi(0, 0, 0)});
  Machine machine(program, 16, 10);
  MachineView view = machine.view();

  std::array<uint8_t, 4> data = {1, 2, 3, 4};
  EXPECT_TRUE(view.write_memory(12, data));
  EXPECT_EQ(machine.memory()[12], 1U);
  EXPECT_EQ(machine.memory()[15], 4U);

  std::array<uint8_t, 4> out = {};
  EXPECT_TRUE(view.read_memory(12, out));
  EXPECT_EQ(out, data);

  std::array<uint8_t, 1> one_byte = {9};
  const std::vector<uint8_t> memory_before_failure(machine.memory().begin(),
                                                   machine.memory().end());
  EXPECT_TRUE(
      view.write_memory(16, std::span<const uint8_t>(one_byte.data(), 0)));
  EXPECT_TRUE(view.read_memory(16, std::span<uint8_t>(one_byte.data(), 0)));
  EXPECT_FALSE(view.write_memory(16, one_byte));
  EXPECT_FALSE(view.write_memory(13, data));
  const std::vector<uint8_t> memory_after_write_failure(
      machine.memory().begin(), machine.memory().end());
  EXPECT_EQ(memory_after_write_failure, memory_before_failure);

  out = {0xaa, 0xbb, 0xcc, 0xdd};
  const std::array<uint8_t, 4> out_before_failure = out;
  EXPECT_FALSE(view.read_memory(13, out));
  EXPECT_EQ(out, out_before_failure);
  EXPECT_FALSE(
      view.write_memory(std::numeric_limits<uint32_t>::max(), one_byte));
  EXPECT_FALSE(view.read_memory(std::numeric_limits<uint32_t>::max(), out));
  const std::vector<uint8_t> memory_after_overflow_failure(
      machine.memory().begin(), machine.memory().end());
  EXPECT_EQ(memory_after_overflow_failure, memory_before_failure);
  EXPECT_EQ(out, out_before_failure);
}

TEST(Rv32DecodeTest, decodes_representative_instruction_families)
{
  struct Case {
    uint32_t word;
    InstructionKind kind;
    Op op;
    int rd;
    int rs1;
    int rs2;
    int32_t immediate;
    uint64_t gas_cost;
  };

  const std::array<Case, 14> cases = {{
      {u_type(0x37, 5, 0x12345000U), InstructionKind::kLui, Op::kLui, 5, -1, -1,
       0x12345000, kBaseInstructionGas},
      {u_type(0x17, 6, 0xfffff000U), InstructionKind::kAuipc, Op::kAuipc, 6, -1,
       -1, -4096, kBaseInstructionGas},
      {j_type(7, -2048), InstructionKind::kJal, Op::kJal, 7, -1, -1, -2048,
       kDirectJumpGas},
      {jalr(8, 9, 12), InstructionKind::kJalr, Op::kJalr, 8, 9, 12, 12,
       kIndirectJumpGas},
      {b_type(0, 1, 2, -16), InstructionKind::kBranch, Op::kBeq, -1, 1, 2, -16,
       kBranchInstructionGas},
      {load(3, 2, 4, 20), InstructionKind::kLoad, Op::kLw, 3, 4, 20, 20,
       kWideLoadGas},
      {s_type(2, 5, 6, -12), InstructionKind::kStore, Op::kSw, -1, 5, 6, -12,
       kBaseInstructionGas},
      {addi(10, 11, -1), InstructionKind::kOpImm, Op::kAddi, 10, 11, 31, -1,
       kBaseInstructionGas},
      {r_type(0, 13, 12, 0, 14), InstructionKind::kOp, Op::kAdd, 14, 12, 13, 0,
       kBaseInstructionGas},
      {r_type(1, 13, 12, 4, 14), InstructionKind::kOp, Op::kDiv, 14, 12, 13, 0,
       kDivInstructionGas},
      {0x0000000fU, InstructionKind::kFence, Op::kFence, 0, 0, 0, 0,
       kBaseInstructionGas},
      {0x00000073U, InstructionKind::kEcall, Op::kEcall, 0, 0, 0, 0,
       kBaseInstructionGas},
      {0x00100073U, InstructionKind::kEbreak, Op::kEbreak, 0, 0, -1, 0,
       kBaseInstructionGas},
      {0xc0001073U, InstructionKind::kUnimp, Op::kUnimp, 0, 0, -1, 0,
       kBaseInstructionGas},
  }};

  for (const auto &test_case : cases) {
    const auto result = decode_instruction(test_case.word);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.instruction.kind, test_case.kind);
    EXPECT_EQ(result.instruction.op, test_case.op);
    if (test_case.rd >= 0) {
      EXPECT_EQ(result.instruction.rd, test_case.rd);
    }
    if (test_case.rs1 >= 0) {
      EXPECT_EQ(result.instruction.rs1, test_case.rs1);
    }
    if (test_case.rs2 >= 0) {
      EXPECT_EQ(result.instruction.rs2, test_case.rs2);
    }
    EXPECT_EQ(result.instruction.immediate, test_case.immediate);
    EXPECT_EQ(result.instruction.gas_cost, test_case.gas_cost);
  }
}

TEST(Rv32GasTest, version_pins_full_cost_table)
{
  // Expected names are `u8"..."` (like the schedule) so the comparison stays
  // on UTF-8 bytes, independent of the execution charset (`-fexec-charset`).
  struct ExpectedGasEntry {
    Op op;
    const char8_t *name;
    uint64_t cost;
  };
  struct ExpectedSyscallGasEntry {
    const char8_t *name;
    uint64_t cost;
  };

  const std::array<ExpectedGasEntry, kOpCount> expected = {{
      {Op::kLui, u8"LUI", 1},     {Op::kAuipc, u8"AUIPC", 1},
      {Op::kJal, u8"JAL", 3},     {Op::kJalr, u8"JALR", 3},
      {Op::kBeq, u8"BEQ", 2},     {Op::kBne, u8"BNE", 2},
      {Op::kBlt, u8"BLT", 2},     {Op::kBge, u8"BGE", 2},
      {Op::kBltu, u8"BLTU", 2},   {Op::kBgeu, u8"BGEU", 2},
      {Op::kLb, u8"LB", 1},       {Op::kLh, u8"LH", 3},
      {Op::kLw, u8"LW", 3},       {Op::kLbu, u8"LBU", 1},
      {Op::kLhu, u8"LHU", 3},     {Op::kSb, u8"SB", 1},
      {Op::kSh, u8"SH", 1},       {Op::kSw, u8"SW", 1},
      {Op::kAddi, u8"ADDI", 1},   {Op::kSlti, u8"SLTI", 1},
      {Op::kSltiu, u8"SLTIU", 1}, {Op::kXori, u8"XORI", 1},
      {Op::kOri, u8"ORI", 1},     {Op::kAndi, u8"ANDI", 1},
      {Op::kSlli, u8"SLLI", 1},   {Op::kSrli, u8"SRLI", 1},
      {Op::kSrai, u8"SRAI", 1},   {Op::kAdd, u8"ADD", 1},
      {Op::kSub, u8"SUB", 1},     {Op::kSll, u8"SLL", 1},
      {Op::kSlt, u8"SLT", 1},     {Op::kSltu, u8"SLTU", 1},
      {Op::kXor, u8"XOR", 1},     {Op::kSrl, u8"SRL", 1},
      {Op::kSra, u8"SRA", 1},     {Op::kOr, u8"OR", 1},
      {Op::kAnd, u8"AND", 1},     {Op::kMul, u8"MUL", 1},
      {Op::kMulh, u8"MULH", 1},   {Op::kMulhsu, u8"MULHSU", 1},
      {Op::kMulhu, u8"MULHU", 1}, {Op::kDiv, u8"DIV", 2},
      {Op::kDivu, u8"DIVU", 2},   {Op::kRem, u8"REM", 2},
      {Op::kRemu, u8"REMU", 2},   {Op::kFence, u8"FENCE", 1},
      {Op::kEcall, u8"ECALL", 1}, {Op::kEbreak, u8"EBREAK", 1},
      {Op::kUnimp, u8"UNIMP", 1},
  }};
  const std::array<ExpectedSyscallGasEntry, 14> expected_syscalls = {{
      {u8"SyscallBase", hogd::kSyscallBaseCost},
      {u8"SHA256HashBlock", hogd::kSHA256HashBlockCost},
      {u8"SHA256Stream", hogd::kSHA256StreamCost},
      {u8"VerifySignatureBase", hogd::kVerifySignatureBaseCost},
      {u8"VerifySignatureBlock", hogd::kVerifySignatureBlockCost},
      {u8"StorageAccess", hogd::kStorageAccessCost},
      {u8"DBRPC", hogd::kDBRPCCost},
      {u8"BytestringBlockBytes", hogd::kBytestringBlockBytes},
      {u8"BytestringWriteBlock", hogd::kBytestringWriteBlockCost},
      {u8"BytestringReadBlock", hogd::kBytestringReadBlockCost},
      {u8"NoSyscall", hogd::kNoSyscallCost},
      {u8"GenericBadSyscall", hogd::kGenericBadSyscallCost},
      {u8"VerifySignatureMlDsa65Base", hogd::kVerifySignatureMlDsa65BaseCost},
      {u8"VerifySignatureMlDsa65Block", hogd::kVerifySignatureMlDsa65BlockCost},
  }};

  EXPECT_EQ(kOpCount, 49U);
  EXPECT_EQ(kGasTableVersion, 4U);
  EXPECT_EQ(kGasScheduleDigest, 0xa958d226df658598ULL);
  ASSERT_EQ(kInstructionGasSchedule.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(kInstructionGasSchedule[i].op, expected[i].op);
    // Both names are `u8"..."`; compare as UTF-8 byte strings via a narrow
    // reinterpret so EXPECT_STREQ stays charset-independent.
    EXPECT_STREQ(reinterpret_cast<const char *>(
                     kInstructionGasSchedule[i].name),
                 reinterpret_cast<const char *>(expected[i].name));
    EXPECT_EQ(kInstructionGasSchedule[i].cost, expected[i].cost);
    EXPECT_EQ(kInstructionGasCosts[i], expected[i].cost);
  }
  ASSERT_EQ(hogd::kSyscallGasCostSchedule.size(), expected_syscalls.size());
  for (size_t i = 0; i < expected_syscalls.size(); ++i) {
    EXPECT_STREQ(reinterpret_cast<const char *>(
                     hogd::kSyscallGasCostSchedule[i].name),
                 reinterpret_cast<const char *>(expected_syscalls[i].name));
    EXPECT_EQ(hogd::kSyscallGasCostSchedule[i].cost, expected_syscalls[i].cost);
  }
}

TEST(Rv32ExecutionTest, keeps_x0_hardwired_and_seeds_sp_zero)
{
  const auto program = make_program({addi(0, 0, 123), addi(1, 0, 7)});
  Machine machine(program, 31, 10);
  EXPECT_EQ(machine.reg(2), 0U);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(0), 0U);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(1), 7U);
}

TEST(Rv32ExecutionTest, executes_arithmetic_logic_and_shift_operations)
{
  const auto program = make_program({
      r_type(0, 2, 1, 0, 3),      // add
      r_type(0x20, 2, 1, 0, 4),   // sub
      r_type(0, 2, 1, 1, 5),      // sll
      r_type(0, 2, 1, 2, 6),      // slt
      r_type(0, 2, 1, 3, 7),      // sltu
      r_type(0, 2, 1, 4, 8),      // xor
      r_type(0, 2, 1, 5, 9),      // srl
      r_type(0x20, 2, 1, 5, 10),  // sra
      r_type(0, 2, 1, 6, 11),     // or
      r_type(0, 2, 1, 7, 12),     // and
      i_type(0x13, 13, 1, 1, 31), // slli
  });
  Machine machine(program, 1024, 100);
  MachineTestPeer::set_reg(machine, 1, 0x80000001U);
  MachineTestPeer::set_reg(machine, 2, 33);

  for (int i = 0; i < 11; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }

  EXPECT_EQ(machine.reg(3), 0x80000022U);
  EXPECT_EQ(machine.reg(4), 0x7fffffe0U);
  EXPECT_EQ(machine.reg(5), 0x00000002U);
  EXPECT_EQ(machine.reg(6), 1U);
  EXPECT_EQ(machine.reg(7), 0U);
  EXPECT_EQ(machine.reg(8), 0x80000020U);
  EXPECT_EQ(machine.reg(9), 0x40000000U);
  EXPECT_EQ(machine.reg(10), 0xc0000000U);
  EXPECT_EQ(machine.reg(11), 0x80000021U);
  EXPECT_EQ(machine.reg(12), 0x00000001U);
  EXPECT_EQ(machine.reg(13), 0x80000000U);
}

TEST(Rv32ExecutionTest, masks_register_shift_amounts)
{
  const auto program = make_program({
      r_type(0, 2, 1, 1, 3),    // sll
      r_type(0, 2, 1, 5, 4),    // srl
      r_type(0x20, 2, 1, 5, 5), // sra
  });
  Machine machine(program, 1024, 10);
  MachineTestPeer::set_reg(machine, 1, 0x80000001U);
  MachineTestPeer::set_reg(machine, 2, 40);

  ASSERT_EQ(machine.step(), StepResult::kContinue);
  ASSERT_EQ(machine.step(), StepResult::kContinue);
  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(3), 0x00000100U);
  EXPECT_EQ(machine.reg(4), 0x00800000U);
  EXPECT_EQ(machine.reg(5), 0xff800000U);
}

TEST(Rv32ExecutionTest, executes_lui_and_auipc_values)
{
  const auto program = make_program(
      {u_type(0x37, 1, 0x12345000U), u_type(0x17, 2, 0x00001000U)});
  Machine machine(program, 1024, 10);

  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(1), 0x12345000U);
  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(2), 0x00001004U);
}

TEST(Rv32ExecutionTest, executes_immediate_operations)
{
  const auto program = make_program({
      i_type(0x13, 3, 2, 1, 0),      // slti
      i_type(0x13, 4, 3, 1, 1),      // sltiu
      i_type(0x13, 5, 4, 1, 0xff),   // xori
      i_type(0x13, 6, 6, 1, 0xf0),   // ori
      i_type(0x13, 7, 7, 1, 0xf0),   // andi
      i_type(0x13, 8, 5, 2, 1),      // srli
      i_type(0x13, 9, 5, 2, 0x401U), // srai
  });
  Machine machine(program, 1024, 100);
  MachineTestPeer::set_reg(machine, 1, 0xffffffffU);
  MachineTestPeer::set_reg(machine, 2, 0x80000000U);

  for (int i = 0; i < 7; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }

  EXPECT_EQ(machine.reg(3), 1U);
  EXPECT_EQ(machine.reg(4), 0U);
  EXPECT_EQ(machine.reg(5), 0xffffff00U);
  EXPECT_EQ(machine.reg(6), 0xffffffffU);
  EXPECT_EQ(machine.reg(7), 0xf0U);
  EXPECT_EQ(machine.reg(8), 0x40000000U);
  EXPECT_EQ(machine.reg(9), 0xc0000000U);
  EXPECT_EQ(machine.gas_remaining(), 93U);
}

TEST(Rv32ExecutionTest, executes_byte_halfword_stores_and_fence)
{
  {
    const auto program = make_program({s_type(0, 1, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 1, 4);
    MachineTestPeer::set_reg(machine, 2, 0xaabbccddU);
    ASSERT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.memory()[4], 0xddU);
    EXPECT_EQ(machine.memory()[5], 0U);
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.gas_remaining(), 9U);
  }
  {
    const auto program = make_program({s_type(1, 1, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 1, 6);
    MachineTestPeer::set_reg(machine, 2, 0xaabbccddU);
    ASSERT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.memory()[6], 0xddU);
    EXPECT_EQ(machine.memory()[7], 0xccU);
    EXPECT_EQ(machine.memory()[8], 0U);
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.gas_remaining(), 9U);
  }
  {
    const auto program = make_program({0x0000000fU});
    Machine machine(program, 16, 10);
    ASSERT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.gas_remaining(), 9U);
  }
}

TEST(Rv32ExecutionTest, ebreak_is_deterministic_runtime_trap)
{
  const auto program = make_program({0x00100073U});
  ASSERT_NE(program, nullptr);
  Machine machine(program, 16, 10);

  EXPECT_EQ(machine.step(), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBreakpoint);
  EXPECT_EQ(machine.pc(), 0U);
  EXPECT_EQ(machine.gas_remaining(), 9U);

  Machine out_of_gas(program, 16, 0);
  EXPECT_EQ(out_of_gas.step(), StepResult::kTrap);
  EXPECT_EQ(out_of_gas.trap(), Trap::kOutOfGas);
  EXPECT_EQ(out_of_gas.pc(), 0U);
  EXPECT_EQ(out_of_gas.gas_remaining(), 0U);
}

TEST(Rv32ExecutionTest, canonical_unimp_decodes_to_kUnimp)
{
  // 0xc0001073 = csrrw x0, cycle, x0: the canonical RISC-V `unimp` trap
  // encoding. Only this exact word is admitted; all other SYSTEM/CSR forms
  // remain kUnsupportedSystemInstruction.
  const auto result = decode_instruction(0xc0001073U);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.instruction.op, Op::kUnimp);
  EXPECT_EQ(result.instruction.kind, InstructionKind::kUnimp);

  const auto program = make_program({0xc0001073U});
  EXPECT_NE(program, nullptr);
}

TEST(Rv32ExecutionTest,
     canonical_unimp_is_deterministic_invalid_instruction_trap)
{
  const auto program = make_program({0xc0001073U});
  ASSERT_NE(program, nullptr);
  Machine machine(program, 16, 10);

  EXPECT_EQ(machine.step(), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kInvalidInstruction);
  EXPECT_EQ(machine.pc(), 0U);
  EXPECT_EQ(machine.gas_remaining(), 9U);

  Machine out_of_gas(program, 16, 0);
  EXPECT_EQ(out_of_gas.step(), StepResult::kTrap);
  EXPECT_EQ(out_of_gas.trap(), Trap::kOutOfGas);
  EXPECT_EQ(out_of_gas.pc(), 0U);
  EXPECT_EQ(out_of_gas.gas_remaining(), 0U);
}

TEST(Rv32ExecutionTest, sign_and_zero_extends_loads)
{
  const auto program = make_program({
      addi(1, 0, 16),
      addi(2, 0, -1),
      s_type(2, 1, 2, 0), // sw
      load(3, 0, 1, 0),   // lb
      load(4, 4, 1, 0),   // lbu
      load(5, 1, 1, 0),   // lh
      load(6, 5, 1, 0),   // lhu
      load(7, 2, 1, 0),   // lw
  });
  Machine machine(program, 64, 100);
  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }
  EXPECT_EQ(machine.reg(3), 0xffffffffU);
  EXPECT_EQ(machine.reg(4), 0xffU);
  EXPECT_EQ(machine.reg(5), 0xffffffffU);
  EXPECT_EQ(machine.reg(6), 0xffffU);
  EXPECT_EQ(machine.reg(7), 0xffffffffU);
}

TEST(Rv32ExecutionTest, branches_and_jumps)
{
  const auto program = make_program({
      addi(1, 0, 1),
      b_type(0, 1, 1, 8), // beq skips next addi
      addi(2, 0, 1),
      addi(2, 0, 2),
      j_type(3, 8), // jal skips next addi
      addi(4, 0, 1),
      addi(4, 0, 4),
  });
  Machine machine(program, 1024, 100);
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }
  EXPECT_EQ(machine.reg(2), 2U);
  EXPECT_EQ(machine.reg(3), 20U);
  EXPECT_EQ(machine.reg(4), 4U);
}

TEST(Rv32ExecutionTest, executes_all_branch_conditions_signed_and_unsigned)
{
  struct Case {
    const char *name;
    uint8_t funct3;
    uint32_t lhs;
    uint32_t rhs;
    uint32_t expected;
  };

  const std::array<Case, 16> cases = {{
      {"beq_taken", 0, 5, 5, 2},
      {"beq_not_taken", 0, 5, 6, 1},
      {"bne_taken", 1, 5, 6, 2},
      {"bne_not_taken", 1, 5, 5, 1},
      {"blt_taken", 4, 0xffffffffU, 1, 2},
      {"blt_not_taken", 4, 1, 0xffffffffU, 1},
      {"blt_equal_not_taken", 4, 5, 5, 1},
      {"bge_taken", 5, 1, 0xffffffffU, 2},
      {"bge_not_taken", 5, 0xffffffffU, 1, 1},
      {"bge_equal_taken", 5, 5, 5, 2},
      {"bltu_taken", 6, 1, 0xffffffffU, 2},
      {"bltu_not_taken", 6, 0xffffffffU, 1, 1},
      {"bltu_equal_not_taken", 6, 5, 5, 1},
      {"bgeu_taken", 7, 0xffffffffU, 1, 2},
      {"bgeu_not_taken", 7, 1, 0xffffffffU, 1},
      {"bgeu_equal_taken", 7, 5, 5, 2},
  }};

  for (const auto &test_case : cases) {
    SCOPED_TRACE(test_case.name);
    const auto program = make_program({
        b_type(test_case.funct3, 1, 2, 8),
        addi(3, 0, 1),
        addi(3, 0, 2),
    });
    Machine machine(program, 1024, 10);
    MachineTestPeer::set_reg(machine, 1, test_case.lhs);
    MachineTestPeer::set_reg(machine, 2, test_case.rhs);

    ASSERT_EQ(machine.step(), StepResult::kContinue);
    ASSERT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.reg(3), test_case.expected);
  }
}

TEST(Rv32ExecutionTest, bad_jump_targets_trap_before_state_mutation)
{
  {
    const auto program = make_program({j_type(1, 2), addi(2, 0, 1)});
    Machine machine(program, 1024, 100);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
    EXPECT_EQ(machine.gas_remaining(), 100 - kDirectJumpGas);
  }
  {
    const auto program = make_program({jalr(1, 2, 0), addi(3, 0, 1)});
    Machine machine(program, 1024, 100);
    MachineTestPeer::set_reg(machine, 2, 3);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
  }
  {
    const auto program =
        make_program({addi(1, 0, 1), b_type(0, 1, 1, 2), addi(2, 0, 1)});
    Machine machine(program, 1024, 100);
    ASSERT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
    EXPECT_EQ(machine.pc(), 4U);
  }
}

TEST(Rv32ExecutionTest, jalr_clears_low_target_bit)
{
  const auto program =
      make_program({jalr(2, 1, 0), addi(3, 0, 1), addi(3, 0, 2)});
  Machine machine(program, 1024, 100);
  MachineTestPeer::set_reg(machine, 1, 9);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.pc(), 8U);
  EXPECT_EQ(machine.reg(2), 4U);
  EXPECT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.reg(3), 2U);
}

TEST(Rv32ExecutionTest, jalrTargetArithmeticWrapsInRv32AddressSpace)
{
  // JALR arithmetic wraps; invalid targets must trap before writing the link.
  {
    const auto program = make_program({jalr(5, 1, 1)});
    Machine machine(program, 16, 100);
    MachineTestPeer::set_reg(machine, 1, std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(machine.step(), StepResult::kContinue);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(5), 4U);
    EXPECT_EQ(machine.gas_remaining(), 100U - kIndirectJumpGas);
  }
  {
    const auto program = make_program({jalr(5, 1, -1)});
    Machine machine(program, 16, 100);
    MachineTestPeer::set_reg(machine, 1, 0);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(5), 0U);
    EXPECT_EQ(machine.gas_remaining(), 100U - kIndirectJumpGas);
  }
}

TEST(Rv32ExecutionTest, fetch_and_data_memory_access_semantics)
{
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, 1024, 10);
    MachineTestPeer::set_pc(machine, 2);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
  }
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, 1024, 10);
    MachineTestPeer::set_pc(machine, 4);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadPc);
  }
  {
    // Unaligned but in-bounds lw: succeeds (no alignment trap).
    const auto program = make_program({load(1, 2, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 2, 1);
    EXPECT_EQ(machine.step(), StepResult::kContinue);
  }
  {
    // Out-of-bounds lw: traps kBadMemory.
    const auto program = make_program({load(1, 2, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 2, 16);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadMemory);
  }
  {
    // Unaligned but in-bounds sw: succeeds (no alignment trap).
    const auto program = make_program({s_type(2, 1, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 1, 2);
    EXPECT_EQ(machine.step(), StepResult::kContinue);
  }
  {
    // Out-of-bounds sw: traps kBadMemory.
    const auto program = make_program({s_type(2, 1, 2, 0)});
    Machine machine(program, 16, 10);
    MachineTestPeer::set_reg(machine, 1, 16);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadMemory);
  }
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, kMemoryCeilingBytes + 1, 10);
    EXPECT_EQ(machine.trap(), Trap::kBadMemory);
    EXPECT_TRUE(machine.halted());
  }
}

TEST(Rv32ExecutionTest, zeroSeededStackUnderflowTrapsWithoutMemoryMutation)
{
  // ABI v3 seeds sp to zero; decrementing it wraps, so the store must trap.
  const auto words = std::array{
      addi(1, 0, 0x7f),
      addi(2, 2, -4),
      s_type(2, 2, 1, 0),
  };
  const auto program = make_program(words);
  Machine canonical(program, /*memory_bytes=*/16, /*gas=*/10);
  EXPECT_EQ(canonical.step(), StepResult::kContinue);
  EXPECT_EQ(canonical.step(), StepResult::kContinue);
  EXPECT_EQ(canonical.reg(2), 0xfffffffcU);
  EXPECT_EQ(canonical.step(), StepResult::kTrap);
  EXPECT_EQ(canonical.trap(), Trap::kBadMemory);
  EXPECT_EQ(canonical.gas_remaining(), 7U);
  EXPECT_TRUE(std::ranges::all_of(canonical.memory(),
                                  [](uint8_t byte) { return byte == 0; }));

  const auto code = code_from_words(words);
  auto pre = ThreadedMachinePreInstantiation::create(code, /*memory=*/{},
                                                     /*memory_start=*/0,
                                                     /*total_memory_size=*/16,
                                                     /*trace=*/false);
  ASSERT_TRUE(pre.has_value());
  ThreadedMachine threaded(*pre, /*gas=*/10);
  EXPECT_EQ(threaded.start(), StepResult::kTrap);
  EXPECT_EQ(threaded.trap(), Trap::kBadMemory);
  EXPECT_EQ(threaded.reg(2), 0xfffffffcU);
  EXPECT_EQ(threaded.gas_remaining(), 7U);
  EXPECT_TRUE(std::ranges::all_of(threaded.memory(),
                                  [](uint8_t byte) { return byte == 0; }));
}

TEST(Rv32ExecutionTest, load_store_traps_debit_gas_without_state_mutation)
{
  struct LoadTrapCase {
    const char *name;
    uint8_t funct3;
    uint32_t address;
    Trap trap;
    // Budget exactly this load's own gas, so gas_remaining() reaching zero
    // means the trapping instruction was charged and nothing more. Budgeting a
    // flat one instruction would make the wide loads trap kOutOfGas before the
    // bounds check and stop testing what this case is named for.
    uint64_t gas;
  };

  const std::array<LoadTrapCase, 5> load_cases = {{
      {"lb_bad_memory", 0, 16, Trap::kBadMemory, kBaseInstructionGas},
      {"lh_bad_memory", 1, 16, Trap::kBadMemory, kWideLoadGas},
      {"lw_bad_memory", 2, 16, Trap::kBadMemory, kWideLoadGas},
      // End-overlap: address in bounds but address+width exceeds memory size.
      {"lh_end_overlap", 1, 15, Trap::kBadMemory, kWideLoadGas}, // 15+2=17 > 16
      {"lw_end_overlap", 2, 13, Trap::kBadMemory, kWideLoadGas}, // 13+4=17 > 16
  }};

  for (const auto &test_case : load_cases) {
    SCOPED_TRACE(test_case.name);
    const auto program = make_program({load(1, test_case.funct3, 2, 0)});
    Machine machine(program, 16, test_case.gas);
    MachineTestPeer::set_reg(machine, 1, 99);
    MachineTestPeer::set_reg(machine, 2, test_case.address);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), test_case.trap);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 99U);
  }

  struct StoreTrapCase {
    const char *name;
    uint8_t funct3;
    uint32_t address;
    Trap trap;
  };

  const std::array<StoreTrapCase, 5> store_cases = {{
      {"sb_bad_memory", 0, 16, Trap::kBadMemory},
      {"sh_bad_memory", 1, 16, Trap::kBadMemory},
      {"sw_bad_memory", 2, 16, Trap::kBadMemory},
      // End-overlap: address in bounds but address+width exceeds memory size.
      {"sh_end_overlap", 1, 15, Trap::kBadMemory}, // 15+2=17 > 16
      {"sw_end_overlap", 2, 13, Trap::kBadMemory}, // 13+4=17 > 16
  }};

  for (const auto &test_case : store_cases) {
    SCOPED_TRACE(test_case.name);
    const auto program = make_program({s_type(test_case.funct3, 1, 2, 0)});
    Machine machine(program, 16, kBaseInstructionGas);
    MachineTestPeer::set_reg(machine, 1, test_case.address);
    MachineTestPeer::set_reg(machine, 2, 0xaabbccddU);
    const std::vector<uint8_t> memory_before(machine.memory().begin(),
                                             machine.memory().end());
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), test_case.trap);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_EQ(machine.pc(), 0U);
    const std::vector<uint8_t> memory_after(machine.memory().begin(),
                                            machine.memory().end());
    EXPECT_EQ(memory_after, memory_before);
  }
}

TEST(Rv32ExecutionTest, running_off_end_traps_as_bad_pc)
{
  const auto program = make_program({addi(1, 0, 1)});
  Machine machine(program, 1024, 10);

  ASSERT_EQ(machine.step(), StepResult::kContinue);
  EXPECT_EQ(machine.pc(), 4U);
  EXPECT_EQ(machine.step(), StepResult::kTrap);
  EXPECT_EQ(machine.trap(), Trap::kBadPc);
}

TEST(Rv32ExecutionTest, implements_m_extension_edge_cases)
{
  const auto program = make_program({
      r_type(1, 2, 1, 0, 3),  // mul
      r_type(1, 2, 1, 1, 4),  // mulh
      r_type(1, 2, 1, 2, 5),  // mulhsu
      r_type(1, 2, 1, 3, 6),  // mulhu
      r_type(1, 2, 1, 4, 7),  // div
      r_type(1, 2, 1, 5, 8),  // divu
      r_type(1, 2, 1, 6, 9),  // rem
      r_type(1, 2, 1, 7, 10), // remu
  });
  Machine machine(program, 1024, 100);
  MachineTestPeer::set_reg(machine, 1, 0x80000000U);
  MachineTestPeer::set_reg(machine, 2, 0xffffffffU);
  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(machine.step(), StepResult::kContinue);
  }
  EXPECT_EQ(machine.reg(3), 0x80000000U);
  EXPECT_EQ(machine.reg(4), 0U);
  EXPECT_EQ(machine.reg(5), 0x80000000U);
  EXPECT_EQ(machine.reg(6), 0x7fffffffU);
  EXPECT_EQ(machine.reg(7), 0x80000000U);
  EXPECT_EQ(machine.reg(8), 0U);
  EXPECT_EQ(machine.reg(9), 0U);
  EXPECT_EQ(machine.reg(10), 0x80000000U);

  const auto div_zero = make_program({
      r_type(1, 2, 1, 4, 3),
      r_type(1, 2, 1, 5, 4),
      r_type(1, 2, 1, 6, 5),
      r_type(1, 2, 1, 7, 6),
  });
  Machine div_zero_machine(div_zero, 1024, 100);
  MachineTestPeer::set_reg(div_zero_machine, 1, 123);
  MachineTestPeer::set_reg(div_zero_machine, 2, 0);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(div_zero_machine.step(), StepResult::kContinue);
  }
  EXPECT_EQ(div_zero_machine.reg(3), 0xffffffffU);
  EXPECT_EQ(div_zero_machine.reg(4), 0xffffffffU);
  EXPECT_EQ(div_zero_machine.reg(5), 123U);
  EXPECT_EQ(div_zero_machine.reg(6), 123U);
}

TEST(Rv32ExecutionTest, debits_gas_before_later_traps_and_mutation)
{
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, 1024, 0);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
  }
  {
    // Unaligned OOB lw: gas debited before trap (kBadMemory, no writeback).
    // Budget the load's own gas, so reaching zero means it was charged exactly
    // once -- a flat one instruction would trap kOutOfGas first.
    const auto program = make_program({load(1, 2, 2, 0)});
    Machine machine(program, 16, kWideLoadGas);
    MachineTestPeer::set_reg(machine, 2, 13);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kBadMemory);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
  }
}

TEST(Rv32GasTest, out_of_gas_saturates_remaining_gas_to_zero)
{
  {
    const auto program = make_program({addi(1, 0, 1)});
    Machine machine(program, 1024, 0);
    EXPECT_EQ(machine.step(), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_EQ(machine.reg(1), 0U);
  }
  {
    struct ExpensiveContinueHost : Host {
      HostResult ecall(MachineView view,
                       const EcallArgs &args) noexcept override
      {
        (void)view;
        (void)args;
        return HostResult::continue_with(123, 10);
      }
    };

    const auto program = make_program({0x00000073U});
    Machine machine(program, 1024, 10);
    ExpensiveContinueHost host;
    EXPECT_EQ(machine.run(host), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.reg(10), 0U);
  }
}

TEST(Rv32GasTest, repeatedEcallsStopBeforeAnUnfundedHostCall)
{
  // Fund two ECALL cycles; both engines must stop before a third host call.
  struct CountingGasHost final : Host {
    explicit CountingGasHost(uint64_t gas_per_call) : gas_per_call(gas_per_call)
    {}

    HostResult ecall(MachineView view, const EcallArgs &args) noexcept override
    {
      (void)view;
      (void)args;
      ++calls;
      return HostResult::continue_with(static_cast<uint32_t>(calls),
                                       gas_per_call);
    }

    uint64_t gas_per_call;
    size_t calls = 0;
  };

  constexpr uint64_t kHostGasPerCall = 2;
  constexpr uint64_t kCompletedCalls = 2;
  constexpr uint64_t kGasLimit =
      kCompletedCalls *
      (kBaseInstructionGas + kHostGasPerCall + kDirectJumpGas);
  const auto words = std::array{0x00000073U, j_type(0, -4)};

  const auto program = make_program(words);
  Machine canonical(program, /*memory_bytes=*/16, kGasLimit);
  CountingGasHost canonical_host(kHostGasPerCall);
  EXPECT_EQ(canonical.run(canonical_host), StepResult::kTrap);
  EXPECT_EQ(canonical.trap(), Trap::kOutOfGas);
  EXPECT_EQ(canonical.gas_remaining(), 0U);
  EXPECT_EQ(canonical_host.calls, kCompletedCalls);
  EXPECT_EQ(canonical.reg(10), kCompletedCalls);

  const auto code = code_from_words(words);
  auto pre = ThreadedMachinePreInstantiation::create(code, /*memory=*/{},
                                                     /*memory_start=*/0,
                                                     /*total_memory_size=*/16,
                                                     /*trace=*/false);
  ASSERT_TRUE(pre.has_value());
  ThreadedMachine threaded(*pre, kGasLimit);
  CountingGasHost threaded_host(kHostGasPerCall);
  EXPECT_EQ(threaded.run(threaded_host), StepResult::kTrap);
  EXPECT_EQ(threaded.trap(), Trap::kOutOfGas);
  EXPECT_EQ(threaded.gas_remaining(), 0U);
  EXPECT_EQ(threaded_host.calls, kCompletedCalls);
  EXPECT_EQ(threaded.reg(10), kCompletedCalls);
}

TEST(Rv32GasTest, charges_mul_and_div_costs_before_mutation)
{
  struct Case {
    const char *name;
    uint8_t funct3;
    uint64_t cost;
    uint32_t expected;
  };

  const std::array<Case, 8> cases = {{
      {"mul", 0, kBaseInstructionGas, 252},
      {"mulh", 1, kBaseInstructionGas, 0},
      {"mulhsu", 2, kBaseInstructionGas, 0},
      {"mulhu", 3, kBaseInstructionGas, 0},
      {"div", 4, kDivInstructionGas, 7},
      {"divu", 5, kDivInstructionGas, 7},
      {"rem", 6, kDivInstructionGas, 0},
      {"remu", 7, kDivInstructionGas, 0},
  }};

  for (const auto &test_case : cases) {
    SCOPED_TRACE(test_case.name);
    const auto program = make_program({r_type(1, 2, 1, test_case.funct3, 3)});
    {
      Machine machine(program, 1024, test_case.cost - 1);
      MachineTestPeer::set_reg(machine, 1, 42);
      MachineTestPeer::set_reg(machine, 2, 6);
      EXPECT_EQ(machine.step(), StepResult::kTrap);
      EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
      EXPECT_EQ(machine.reg(3), 0U);
      EXPECT_EQ(machine.gas_remaining(), 0U);
    }
    {
      Machine machine(program, 1024, test_case.cost);
      MachineTestPeer::set_reg(machine, 1, 42);
      MachineTestPeer::set_reg(machine, 2, 6);
      EXPECT_EQ(machine.step(), StepResult::kContinue);
      EXPECT_EQ(machine.reg(3), test_case.expected);
      EXPECT_EQ(machine.gas_remaining(), 0U);
    }
  }
}

TEST(Rv32ExecutionTest, handles_ecall_step_completion_and_run)
{
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 1024, kBaseInstructionGas);
    MachineTestPeer::set_reg(machine, 10, 1);
    MachineTestPeer::set_reg(machine, 17, 99);
    EXPECT_EQ(machine.step(), StepResult::kEcall);
    const auto pending_ecall = machine.pending_ecall();
    ASSERT_TRUE(pending_ecall.has_value());
    const EcallArgs ecall = pending_ecall.value_or(EcallArgs{});
    EXPECT_EQ(ecall.a0, 1U);
    EXPECT_EQ(ecall.a7, 99U);
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_EQ(machine.complete_ecall(HostResult::continue_with(55)),
              StepResult::kContinue);
    EXPECT_EQ(machine.reg(10), 55U);
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 1024, 100);
    MachineTestPeer::set_reg(machine, 10, 7);
    MachineTestPeer::set_reg(machine, 17, 42);
    HaltHost host;
    EXPECT_EQ(machine.run(host), StepResult::kHalted);
    EXPECT_EQ(host.calls, 1);
    EXPECT_EQ(host.last_args.a0, 7U);
    EXPECT_EQ(host.last_args.a7, 42U);
    EXPECT_EQ(host.last_pc, 4U);
    EXPECT_EQ(machine.trap(), Trap::kNone);
    EXPECT_EQ(machine.halt_code(), 77U);
  }
  {
    const auto program =
        make_program({0x00000073U, addi(1, 10, 1), 0x00000073U});
    Machine machine(program, 1024, 100);
    ContinueThenHaltHost host;
    EXPECT_EQ(machine.run(host), StepResult::kHalted);
    EXPECT_EQ(host.calls, 2);
    EXPECT_EQ(machine.reg(1), 124U);
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 1024, 100);
    TrapHost host;
    EXPECT_EQ(machine.run(host), StepResult::kTrap);
    EXPECT_EQ(host.calls, 1);
    EXPECT_EQ(machine.trap(), Trap::kHostTrap);
    EXPECT_EQ(machine.host_trap_code(), 77U);
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 1024, 100);
    MachineTestPeer::set_reg(machine, 10, 7);
    OutOfGasHost host;
    EXPECT_EQ(machine.run(host), StepResult::kTrap);
    EXPECT_EQ(host.calls, 1);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.reg(10), 7U);
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_FALSE(machine.pending_ecall().has_value());
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 16, 100);
    MutatingOutOfGasHost host;
    EXPECT_EQ(machine.run(host), StepResult::kTrap);
    EXPECT_EQ(host.calls, 1);
    EXPECT_TRUE(host.wrote);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.memory()[4], 0xaaU); // host writes persist on host-OOG
    EXPECT_EQ(machine.memory()[7], 0xddU); // (matches host-trap; no rollback)
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_FALSE(machine.pending_ecall().has_value());
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 16, 100);
    MutatingOutOfGasHost host;
    EXPECT_EQ(machine.step(), StepResult::kEcall);
    const auto pending_ecall = machine.pending_ecall();
    ASSERT_TRUE(pending_ecall.has_value());
    const HostResult result =
        host.ecall(machine.view(), pending_ecall.value_or(EcallArgs{}));
    EXPECT_EQ(machine.memory()[4], 0xaaU);
    EXPECT_EQ(machine.complete_ecall(result), StepResult::kTrap);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.memory()[4], 0xaaU); // no rollback
    EXPECT_EQ(machine.memory()[7], 0xddU);
    EXPECT_EQ(machine.pc(), 4U);
    EXPECT_EQ(machine.gas_remaining(), 0U);
    EXPECT_FALSE(machine.pending_ecall().has_value());
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 16, 0);
    MachineTestPeer::set_reg(machine, 10, 7);
    TrapHost host;
    EXPECT_EQ(machine.run(host), StepResult::kTrap);
    EXPECT_EQ(host.calls, 0);
    EXPECT_EQ(machine.trap(), Trap::kOutOfGas);
    EXPECT_EQ(machine.pc(), 0U);
    EXPECT_EQ(machine.reg(10), 7U);
    EXPECT_FALSE(machine.pending_ecall().has_value());
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 16, 100);
    MutatingTrapHost host;
    EXPECT_EQ(machine.run(host), StepResult::kTrap);
    EXPECT_EQ(host.calls, 1);
    EXPECT_TRUE(host.wrote);
    EXPECT_EQ(machine.trap(), Trap::kHostTrap);
    EXPECT_EQ(machine.memory()[4], 0xaaU);
    EXPECT_EQ(machine.memory()[7], 0xddU);
  }
  {
    const auto program = make_program({0x00000073U});
    Machine machine(program, 16, 100);
    MemoryHost host;
    EXPECT_EQ(machine.run(host), StepResult::kHalted);
    EXPECT_TRUE(host.wrote);
    EXPECT_EQ(machine.memory()[8], 1U);
    EXPECT_EQ(machine.memory()[11], 4U);
  }
}

TEST(Rv32ConformanceTest, runs_representative_loop_program)
{
  const auto program = make_program({
      addi(1, 0, 5),         // counter
      addi(2, 0, 0),         // sum
      r_type(0, 1, 2, 0, 2), // sum += counter
      addi(1, 1, -1),        // --counter
      b_type(1, 1, 0, -8),   // loop while counter != 0
      0x00000073U,           // ecall halt
  });
  Machine machine(program, 1024, 100);
  HaltHost host;
  EXPECT_EQ(machine.run(host), StepResult::kHalted);
  EXPECT_EQ(machine.reg(2), 15U);
  EXPECT_EQ(host.calls, 1);
}

TEST(Rv32ConformanceTest, randomized_store_bounds_are_deterministic)
{
  const auto program = make_program({s_type(2, 1, 2, 0)});
  std::mt19937 rng(0x12345678);
  std::uniform_int_distribution<uint32_t> address_dist(0, 40);

  for (int i = 0; i < 128; ++i) {
    const uint32_t address = address_dist(rng);
    Machine machine(program, 32, 10);
    MachineTestPeer::set_reg(machine, 1, address);
    MachineTestPeer::set_reg(machine, 2, 0xaabbccddU);
    const StepResult result = machine.step();
    if (!memory_range_in_bounds(machine.memory().size(), address, 4)) {
      EXPECT_EQ(result, StepResult::kTrap);
      EXPECT_EQ(machine.trap(), Trap::kBadMemory);
    }
    else {
      ASSERT_EQ(result, StepResult::kContinue);
      const auto memory = machine.memory();
      EXPECT_EQ(memory[address], 0xddU);
      EXPECT_EQ(memory[address + 1], 0xccU);
      EXPECT_EQ(memory[address + 2], 0xbbU);
      EXPECT_EQ(memory[address + 3], 0xaaU);
    }
  }
}

} // namespace
} // namespace hogd::rv32

// NOLINTEND(google-readability-avoid-underscore-in-googletest-name,modernize-use-designated-initializers,readability-suspicious-call-argument,bugprone-random-generator-seed,bugprone-unchecked-optional-access)
