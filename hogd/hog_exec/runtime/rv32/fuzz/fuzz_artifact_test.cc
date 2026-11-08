#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *, size_t, size_t,
                                          unsigned int);

// Keep the raw-mutation fallback unchanged so these tests isolate the custom
// mutator's envelope accounting without linking libFuzzer's main.
extern "C" size_t
// NOLINTNEXTLINE(readability-identifier-naming)
LLVMFuzzerMutate(uint8_t *, size_t size, size_t max_size)
{
  return std::min(size, max_size);
}

namespace hogd::rv32 {
namespace {

TEST(Rv32ArtifactMutator, StructuredMutationsFitAtV5AndV6SizeLimits)
{
  auto v6 = test_artifact::build_artifact({test_program::addi(0, 0, 0)},
                                          std::array<uint8_t, 16>{});
  auto v5 = v6;
  v5.resize(v5.size() - 4);
  store_u32_le(v5, kArtifactFormatVersionOffset, kArtifactVersion5);
  // Seeds select code shape, memory shape, zero memory, and memory limit.
  for (const auto &input : {v5, v6}) {
    for (size_t extra : {0U, 1U, 3U, 4U}) {
      for (unsigned int seed : {1U, 4U, 13U, 8U}) {
        const size_t max_size = input.size() + extra;
        std::vector<uint8_t> output(max_size + 1, 0xAA);
        std::ranges::copy(input, output.begin());
        const auto size = LLVMFuzzerCustomMutator(output.data(), input.size(),
                                                  max_size, seed);
        ASSERT_LE(size, max_size);
        EXPECT_EQ(output[max_size], 0xAA);
        const auto bytes = std::span<const uint8_t>(output).first(size);
        ASSERT_GE(size, kArtifactHeaderBytes + 4);
        const size_t code_size = load_u32_le(bytes, kArtifactCodeSizeOffset);
        const size_t memory_size =
            load_u32_le(bytes, kArtifactInitialMemorySizeOffset);
        EXPECT_EQ(size, kArtifactHeaderBytes + code_size + memory_size + 4);
        EXPECT_EQ(load_u32_le(bytes, size - 4), 0U);
      }
    }
  }
}

TEST(Rv32ArtifactMutator, TooSmallForV6UsesRawMutation)
{
  for (size_t max_size = kArtifactHeaderBytes;
       max_size < kArtifactHeaderBytes + 8; ++max_size) {
    std::vector<uint8_t> bytes(max_size, 0xAA);
    EXPECT_EQ(LLVMFuzzerCustomMutator(bytes.data(), bytes.size(), max_size, 8),
              max_size);
    EXPECT_TRUE(
        std::ranges::all_of(bytes, [](uint8_t b) { return b == 0xAA; }));
  }
}

TEST(Rv32ArtifactMutator, EmptyMemoryOnlyGrowsWhenSpaceRemains)
{
  const auto input =
      test_artifact::build_artifact({test_program::addi(0, 0, 0)}, {});
  for (size_t extra : {0U, 1U}) {
    // Seeds select memory mutation with and without digest repair.
    for (unsigned int seed : {14U, 16U}) {
      const size_t max_size = input.size() + extra;
      std::vector<uint8_t> output(max_size + 1, 0xAA);
      std::ranges::copy(input, output.begin());
      const auto size =
          LLVMFuzzerCustomMutator(output.data(), input.size(), max_size, seed);
      EXPECT_EQ(size, max_size);
      EXPECT_EQ(output[max_size], 0xAA);
      EXPECT_EQ(load_u32_le(output, kArtifactInitialMemorySizeOffset), extra);
      EXPECT_EQ(load_u32_le(output, size - 4), 0U);
    }
  }
}

} // namespace
} // namespace hogd::rv32
