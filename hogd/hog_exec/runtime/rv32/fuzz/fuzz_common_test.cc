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

TEST(Rv32FuzzCarveEngineDiff, ClampsGasAndMemAndForcesWordAlignedCode)
{
  std::vector<uint8_t> data(8, 0xFF); // gas u64 = all ones
  const std::array<uint8_t, 4> mem = {0xFF, 0xFF, 0xFF, 0xFF};
  data.insert(data.end(), mem.begin(), mem.end()); // mem u32 = all ones
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
  std::vector<uint8_t> data(8 + 4, 0); // gas + mem headers, zero code bytes
  data.push_back(0x13);                // 1 stray byte, < one word
  EXPECT_FALSE(carve_engine_diff_input(data).has_value());
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
