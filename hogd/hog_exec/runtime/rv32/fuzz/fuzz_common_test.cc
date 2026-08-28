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

TEST(Rv32FuzzDecoderParity, ProductionGateAgreesOnAcceptRejectOverCorpus)
{
  // The scan helpers above are harness-local; production runs
  // ThreadedMachinePreInstantiation::create. Its accept/reject verdict must
  // track the canonical decoder on every input the harness can carve.
  for (uint64_t seed = 0; seed < 32; ++seed) {
    const std::vector<uint32_t> words =
        test_corpus::generate_engine_only_program(seed);
    const std::vector<uint8_t> code = test_program::code_from_words(words);
    EXPECT_EQ(production_scan_error(code).has_value(),
              canonical_scan_error(code).has_value())
        << "seed " << seed;
  }
}

TEST(Rv32FuzzDecoderParity, ProductionGateMatchesErrorOnSingleBadWord)
{
  // With exactly one invalid word there is no ordering ambiguity, so the
  // production gate must report the same error code as the canonical decoder.
  const std::vector<uint32_t> singles = {
      0x00000000U, // compressed
      0x0000000bU, // unknown opcode (custom-0)
      0x30200073U, // SYSTEM, not ecall/ebreak/unimp
  };
  for (const uint32_t word : singles) {
    const std::vector<uint8_t> code = test_program::code_from_words(
        {test_program::addi(1, 0, 5), word, test_program::addi(2, 0, 6)});
    const auto canonical = canonical_scan_error(code);
    ASSERT_TRUE(canonical.has_value()) << std::hex << word;
    EXPECT_EQ(production_scan_error(code), canonical) << std::hex << word;
  }
}

TEST(Rv32FuzzDecoderParity, ProductionGateRejectsMultiErrorProgram)
{
  // Two invalid words of different kinds. The canonical decoder scans forward
  // and reports the first (kUnsupportedInstruction); create()'s backward
  // lowering reports the last (kUnsupportedSystemInstruction). Both errors
  // collapse into the same deterministic deploy rejection, so only the
  // accept/reject verdict is pinned -- deliberately NOT the error code.
  const std::vector<uint8_t> code =
      test_program::code_from_words({0xffffffffU, 0x00200073U});
  const auto canonical = canonical_scan_error(code);
  ASSERT_TRUE(canonical.has_value());
  EXPECT_EQ(canonical, ProgramError::kUnsupportedInstruction);
  EXPECT_TRUE(production_scan_error(code).has_value());
}

TEST(Rv32FuzzCarveEngineDiff, ClampsGasAndMemAndForcesWordAlignedCode)
{
  std::vector<uint8_t> data(8, 0xFF); // gas u64 = all ones
  const std::array<uint8_t, 4> mem = {0xFF, 0xFF, 0xFF, 0xFF};
  data.insert(data.end(), mem.begin(), mem.end()); // mem u32 = all ones
  const std::array<uint8_t, 4> knob = {0, 0, 0, 0};
  data.insert(data.end(), knob.begin(), knob.end()); // run-gas-cap knob
  // 6 trailing bytes -> exactly one 4-byte word of code (last 2 dropped).
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
}

TEST(Rv32FuzzCarveEngineDiff, ReturnsNulloptWhenNoFullCodeWord)
{
  std::vector<uint8_t> data(8 + 4 + 4, 0); // gas + mem + cap-knob headers
  data.push_back(0x13);                    // 1 stray byte, < one word
  EXPECT_FALSE(carve_engine_diff_input(data).has_value());
}

TEST(Rv32FuzzCarveEngineDiff, RunGasCapKnobBiasesSmallAndKeepsProductionCap)
{
  // Knob 0 (also what any too-short input reads) keeps the production cap;
  // every mapped cap stays within [kFuzzMinRunGasCap, kMaxRunGas].
  EXPECT_EQ(run_gas_cap_from_knob(0), kMaxRunGas);
  EXPECT_EQ(run_gas_cap_from_knob(4), kMaxRunGas); // any knob % 4 == 0
  EXPECT_EQ(run_gas_cap_from_knob(1), kFuzzMinRunGasCap);
  for (uint32_t knob = 0; knob < 512; ++knob) {
    const uint32_t cap = run_gas_cap_from_knob(knob);
    EXPECT_GE(cap, kFuzzMinRunGasCap) << "knob " << knob;
    EXPECT_LE(cap, kMaxRunGas) << "knob " << knob;
  }
  // The knob is the third header field: [u64 gas][u32 mem][u32 knob][code].
  std::vector<uint8_t> data(8 + 4, 0);
  const std::array<uint8_t, 4> knob_one = {1, 0, 0, 0};
  data.insert(data.end(), knob_one.begin(), knob_one.end());
  data.insert(data.end(), {0x13, 0x00, 0x00, 0x00}); // one code word
  const auto in = carve_engine_diff_input(data);
  ASSERT_TRUE(in.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(in->max_run_gas, kFuzzMinRunGasCap);
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
