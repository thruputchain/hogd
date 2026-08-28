#include "hogd/hog_exec/runtime/rv32/fuzz/fuzz_common.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_corpus.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace hogd::rv32::fuzz {
namespace {

TEST(Rv32FuzzCommon, ByteReaderReadsLittleEndianAndZeroFillsPastEnd)
{
  const std::array<uint8_t, 4> bytes = {0x01, 0x02, 0x03, 0x04};
  ByteReader reader{std::span<const uint8_t>(bytes)};
  EXPECT_EQ(reader.u32(), 0x04030201U);
  // Exhausted: further reads return zero, remaining() stays 0, no over-read.
  EXPECT_EQ(reader.remaining(), 0U);
  EXPECT_EQ(reader.u64(), 0U);
  EXPECT_TRUE(reader.rest().empty());
}

TEST(Rv32FuzzCommon, ByteReaderRestReturnsUnconsumedTail)
{
  const std::array<uint8_t, 6> bytes = {0, 0, 0, 0, 0xAA, 0xBB};
  ByteReader reader{std::span<const uint8_t>(bytes)};
  (void)reader.u32();
  EXPECT_EQ(reader.remaining(), 2U);
  const std::span<const uint8_t> tail = reader.rest();
  ASSERT_EQ(tail.size(), 2U);
  EXPECT_EQ(tail[0], 0xAAU);
  EXPECT_EQ(tail[1], 0xBBU);
}

TEST(Rv32FuzzCommon, ConstantsHoldTheTraceBoundInvariant)
{
  EXPECT_LT(kFuzzGasCap, kFuzzTraceMaxSteps);
  EXPECT_LE(kFuzzMemCap, kMaxMemoryBytes);
}

TEST(Rv32FuzzDecoderParity, AgreeOnValidStream)
{
  const std::vector<uint8_t> code = test_program::code_from_words(
      {test_program::addi(1, 0, 5), test_program::op(2, 1, 1, 0, 0)});
  EXPECT_EQ(canonical_scan_error(code), std::nullopt);
  EXPECT_EQ(threaded_scan_error(code), std::nullopt);
}

TEST(Rv32FuzzDecoderParity, AgreeOnCompressedWord)
{
  // Low two bits != 0b11 -> compressed, rejected by both decoders.
  const std::vector<uint8_t> code =
      test_program::code_from_words({0x00000000U});
  EXPECT_EQ(canonical_scan_error(code),
            ProgramError::kUnsupportedCompressedInstruction);
  EXPECT_EQ(threaded_scan_error(code),
            ProgramError::kUnsupportedCompressedInstruction);
}

TEST(Rv32FuzzDecoderParity, AgreeOnUnknownOpcode)
{
  // Opcode 0x0b (custom-0), full-width but unsupported.
  const std::vector<uint8_t> code =
      test_program::code_from_words({0x0000000bU});
  EXPECT_EQ(canonical_scan_error(code), ProgramError::kUnsupportedInstruction);
  EXPECT_EQ(threaded_scan_error(code), ProgramError::kUnsupportedInstruction);
}

TEST(Rv32FuzzDecoderParity, AgreeOnBadSystemInstruction)
{
  // SYSTEM opcode (0x73) that is not ecall/ebreak/canonical-unimp.
  const std::vector<uint8_t> code =
      test_program::code_from_words({0x30200073U});
  EXPECT_EQ(canonical_scan_error(code),
            ProgramError::kUnsupportedSystemInstruction);
  EXPECT_EQ(threaded_scan_error(code),
            ProgramError::kUnsupportedSystemInstruction);
}

TEST(Rv32FuzzDecoderParity, AgreeOverEngineCorpus)
{
  for (uint64_t seed = 0; seed < 32; ++seed) {
    const std::vector<uint32_t> words =
        test_corpus::generate_engine_only_program(seed);
    const std::vector<uint8_t> code = test_program::code_from_words(words);
    EXPECT_EQ(canonical_scan_error(code), threaded_scan_error(code))
        << "seed " << seed;
  }
}

// [u64 gas][u32 mem][u64 method_id][u64 host_seed][u32 start][u32 img_len].
constexpr size_t kEngineDiffHeaderBytes = 8 + 4 + 8 + 8 + 4 + 4;

void
append_u32_le(std::vector<uint8_t> &out, uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

TEST(Rv32FuzzCarveEngineDiff, ClampsGasAndMemAndForcesWordAlignedCode)
{
  // All-ones header: gas/mem clamp to their caps; the huge start/img_len knobs
  // must still leave a full code word (the image may never eat the last word).
  std::vector<uint8_t> data(kEngineDiffHeaderBytes, 0xFF);
  // 6 trailing bytes -> at most one 4-byte word of code survives.
  for (int i = 0; i < 6; ++i) {
    data.push_back(static_cast<uint8_t>(0x13 + i));
  }
  const auto in = carve_engine_diff_input(data);
  ASSERT_TRUE(in.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const EngineDiffInput &input = in.value();
  EXPECT_LE(input.gas_budget, kFuzzGasCap);
  EXPECT_LE(input.memory_bytes, kFuzzMemCap);
  ASSERT_EQ(input.code.size() % kInstructionBytes, 0U);
  EXPECT_EQ(input.code.size(), 4U);
  EXPECT_LE(input.memory_start + input.initial_memory.size(),
            input.memory_bytes);
}

TEST(Rv32FuzzCarveEngineDiff, ReturnsNulloptWhenNoFullCodeWord)
{
  std::vector<uint8_t> data(kEngineDiffHeaderBytes, 0); // header, no code
  data.push_back(0x13); // 1 stray byte, < one word
  EXPECT_FALSE(carve_engine_diff_input(data).has_value());
}

TEST(Rv32FuzzCarveEngineDiff, CarriesMethodIdAndHostSeedVerbatim)
{
  std::vector<uint8_t> data(8 + 4, 0); // gas = 0, mem = 0
  const std::array<uint8_t, 8> method_id = {1, 2, 3, 4, 5, 6, 7, 8};
  data.insert(data.end(), method_id.begin(), method_id.end());
  const std::array<uint8_t, 8> host_seed = {9, 10, 11, 12, 13, 14, 15, 16};
  data.insert(data.end(), host_seed.begin(), host_seed.end());
  append_u32_le(data, 0); // start knob
  append_u32_le(data, 0); // img_len knob
  data.insert(data.end(), {0x13, 0x00, 0x00, 0x00});
  const auto in = carve_engine_diff_input(data);
  ASSERT_TRUE(in.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(in->method_id, 0x0807060504030201ULL);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(in->host_seed, 0x100f0e0d0c0b0a09ULL);
}

TEST(Rv32FuzzCarveEngineDiff, SplitsImageAndCodeAtCarvedLength)
{
  std::vector<uint8_t> data(8, 0); // gas = 0
  append_u32_le(data, 64);         // mem = 64 (< cap: taken verbatim)
  data.insert(data.end(), 16, 0);  // method_id + host_seed
  append_u32_le(data, 8);          // start = 8 % (64 + 1) = 8
  append_u32_le(data, 4);          // img_len = 4 % (max_img + 1) = 4
  data.insert(data.end(), {0xAA, 0xBB, 0xCC, 0xDD}); // image
  data.insert(data.end(), {0x13, 0x00, 0x00, 0x00,   // two code words
                           0x93, 0x00, 0x00, 0x00});
  const auto in = carve_engine_diff_input(data);
  ASSERT_TRUE(in.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const EngineDiffInput &input = in.value();
  EXPECT_EQ(input.memory_start, 8U);
  EXPECT_EQ(input.initial_memory,
            (std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD}));
  EXPECT_EQ(input.code.size(), 8U);
  EXPECT_EQ(input.code[0], 0x13U);
}

TEST(Rv32FuzzCarveEngineDiff, EmptyImageCanonicalizesStartToZero)
{
  std::vector<uint8_t> data(8, 0); // gas = 0
  append_u32_le(data, 64);         // mem = 64
  data.insert(data.end(), 16, 0);  // method_id + host_seed
  append_u32_le(data, 8);          // nonzero start knob
  append_u32_le(data, 0);          // img_len = 0
  data.insert(data.end(), {0x13, 0x00, 0x00, 0x00});
  const auto in = carve_engine_diff_input(data);
  ASSERT_TRUE(in.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_TRUE(in->initial_memory.empty());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(in->memory_start, 0U);
}

TEST(Rv32FuzzScriptedHost, ScheduleIsDeterministicAcrossInstances)
{
  std::array<uint32_t, kRegisterCount> regs = {};
  std::vector<uint8_t> mem_a(64, 0);
  std::vector<uint8_t> mem_b(64, 0);
  ScriptedHost a(/*seed=*/42);
  ScriptedHost b(/*seed=*/42);
  for (int i = 0; i < 16; ++i) {
    const HostResult ra =
        a.ecall(MachineView(0, 100, regs, mem_a), EcallArgs{});
    const HostResult rb =
        b.ecall(MachineView(0, 100, regs, mem_b), EcallArgs{});
    EXPECT_EQ(ra.status, rb.status) << "call " << i;
    EXPECT_EQ(ra.gas_consumed, rb.gas_consumed) << "call " << i;
    if (ra.status == HostStatus::kContinue &&
        rb.status == HostStatus::kContinue) {
      EXPECT_EQ(ra.continue_value(), rb.continue_value()) << "call " << i;
    }
  }
  // Identical schedules make identical guest-memory writes.
  EXPECT_EQ(mem_a, mem_b);
}

TEST(Rv32FuzzScriptedHost, NeverReturnsHostTrap)
{
  // The threaded ecall seam aborts on HostStatus::kTrap by contract, so the
  // schedule must never produce one.
  std::array<uint32_t, kRegisterCount> regs = {};
  std::vector<uint8_t> mem(64, 0);
  for (uint64_t seed = 0; seed < 8; ++seed) {
    ScriptedHost host(seed);
    for (int i = 0; i < 64; ++i) {
      const HostResult result =
          host.ecall(MachineView(0, 100, regs, mem), EcallArgs{});
      EXPECT_NE(result.status, HostStatus::kTrap);
    }
  }
}

TEST(Rv32FuzzScriptedHost, ScheduleCoversAllAllowedStatuses)
{
  // Sanity that the knob distribution actually reaches every allowed arm:
  // continue, halt, host out-of-gas, and an over-cap gas debit.
  std::array<uint32_t, kRegisterCount> regs = {};
  std::vector<uint8_t> mem(64, 0);
  bool saw_continue = false;
  bool saw_halt = false;
  bool saw_oog = false;
  bool saw_big_debit = false;
  ScriptedHost host(/*seed=*/7);
  for (int i = 0; i < 256; ++i) {
    const HostResult result =
        host.ecall(MachineView(0, 100, regs, mem), EcallArgs{});
    saw_continue = saw_continue || result.status == HostStatus::kContinue;
    saw_halt = saw_halt || result.status == HostStatus::kHalt;
    saw_oog = saw_oog || result.status == HostStatus::kOutOfGas;
    saw_big_debit = saw_big_debit || result.gas_consumed > kFuzzGasCap;
  }
  EXPECT_TRUE(saw_continue);
  EXPECT_TRUE(saw_halt);
  EXPECT_TRUE(saw_oog);
  EXPECT_TRUE(saw_big_debit);
}

TEST(Rv32FuzzCarveArtifact, ForcesNonEmptyCodeAndClampsMemoryToRemaining)
{
  // Headers: start u32, mem_len u32 (huge), extra u32. Then a tiny 3-byte tail:
  // there is no full code word to reserve, so code is forced non-empty.
  std::vector<uint8_t> data;
  const std::array<uint8_t, 4> zero = {0, 0, 0, 0};
  data.insert(data.end(), zero.begin(), zero.end()); // start = 0
  const std::array<uint8_t, 4> big = {0xFF, 0xFF, 0xFF, 0xFF};
  data.insert(data.end(), big.begin(), big.end());   // mem_len = huge
  data.insert(data.end(), zero.begin(), zero.end()); // extra = 0
  data.push_back(0xAA);
  data.push_back(0xBB);
  data.push_back(0xCC); // 3-byte tail
  const auto fx = carve_artifact_fixup(data);
  ASSERT_TRUE(fx.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const ArtifactFixup &fixup = fx.value();
  EXPECT_LE(fixup.initial_memory.size(), 3U);
  EXPECT_LE(std::size_t{fixup.initial_memory_start} +
                fixup.initial_memory.size(),
            std::size_t{kMaxMemoryBytes});
  EXPECT_FALSE(fixup.code_words.empty()); // forced non-empty
  EXPECT_GE(fixup.memory_bytes,
            std::max<uint32_t>(1, fixup.initial_memory_start +
                                      static_cast<uint32_t>(
                                          fixup.initial_memory.size())));
  EXPECT_LE(fixup.memory_bytes, kMaxMemoryBytes);
}

TEST(Rv32FuzzCarveArtifact, ReservesFullWordForFuzzerCodeWhenAvailable)
{
  std::vector<uint8_t> data;
  const std::array<uint8_t, 4> zero = {0, 0, 0, 0};
  const std::array<uint8_t, 4> big = {0xFF, 0xFF, 0xFF, 0xFF};
  data.insert(data.end(), zero.begin(), zero.end()); // start = 0
  data.insert(data.end(), big.begin(), big.end());   // large mem_len knob
  data.insert(data.end(), zero.begin(), zero.end()); // extra = 0
  data.insert(data.end(), {0xaa, 0xbb, 0xcc, 0xdd, 0x13, 0x00, 0x10, 0x00});

  const auto fx = carve_artifact_fixup(data);
  ASSERT_TRUE(fx.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const ArtifactFixup &fixup = fx.value();
  ASSERT_FALSE(fixup.code_words.empty());
  EXPECT_EQ(fixup.code_words.back(), 0x00100013U);
}

TEST(Rv32FuzzCarveArtifact, ReturnsNulloptWhenNoPayload)
{
  std::vector<uint8_t> data(12, 0); // headers only, no tail
  EXPECT_FALSE(carve_artifact_fixup(data).has_value());
}

TEST(Rv32FuzzCarveArtifact, EmptyInitialMemoryUsesCanonicalStartZero)
{
  std::vector<uint8_t> data;
  const std::array<uint8_t, 4> start = {5, 0, 0, 0};
  const std::array<uint8_t, 4> zero = {0, 0, 0, 0};
  data.insert(data.end(), start.begin(), start.end()); // nonzero start knob
  data.insert(data.end(), zero.begin(), zero.end());   // mem_len = 0
  data.insert(data.end(), zero.begin(), zero.end());   // extra = 0
  data.insert(data.end(), {0x13, 0x00, 0x00, 0x00});   // one code word

  const auto fx = carve_artifact_fixup(data);
  ASSERT_TRUE(fx.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const ArtifactFixup &fixup = fx.value();
  EXPECT_TRUE(fixup.initial_memory.empty());
  EXPECT_EQ(fixup.initial_memory_start, 0U);
  EXPECT_GE(fixup.memory_bytes, 1U);
  EXPECT_FALSE(fixup.code_words.empty());
}

} // namespace
} // namespace hogd::rv32::fuzz
