#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/rv32/encoding.h"
#include "hogd/hogexec/runtime/rv32/musttail.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/test_corpus.h"
#include "hogd/hogexec/runtime/rv32/threaded_program.h"

namespace hogd::rv32 {
namespace {

// Build a byte image (little-endian) from instruction words.
std::vector<uint8_t>
words_to_bytes(std::span<const uint32_t> words)
{
  std::vector<uint8_t> bytes(words.size() * kInstructionBytes);
  for (size_t i = 0; i < words.size(); ++i) {
    store_u32_le(bytes, i * kInstructionBytes, words[i]);
  }
  return bytes;
}

TEST(Rv32Threaded, decodeMatchesCanonicalOverCorpus)
{
  auto check = [](const std::vector<uint32_t> &words, uint64_t seed,
                  const char *kind) {
    const std::vector<uint8_t> bytes = words_to_bytes(words);

    const auto canonical = Program::create(bytes);
    ASSERT_TRUE(canonical.has_value()) << kind << " seed " << seed;
    const auto threaded = ThreadedProgram::create(bytes);
    ASSERT_TRUE(threaded.has_value()) << kind << " seed " << seed;

    ASSERT_EQ(threaded->instruction_count(), canonical->instruction_count());
    for (size_t i = 0; i < words.size(); ++i) {
      const DecodedInstruction &d = canonical->instruction_at_aligned_pc(
          static_cast<uint32_t>(i) * kInstructionBytes);
      const Instr &t = threaded->code()[i];
      EXPECT_EQ(t.op, d.op) << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.rd, d.rd) << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.rs1, d.rs1) << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.rs2, d.rs2) << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.imm, d.immediate)
          << kind << " seed " << seed << " instr " << i;
      EXPECT_EQ(t.gas, d.gas_cost)
          << kind << " seed " << seed << " instr " << i;
    }
  };

  // Both generators: register-only covers SLLI/SRLI/SRAI (funct3 1/5) and the
  // full ALU/branch/jal surface; memory adds loads/stores. Together they
  // exercise every decode path the corpus can reach.
  for (uint64_t seed = 0; seed < 16; ++seed) {
    check(test_corpus::generate_register_only_program(seed, /*body_len=*/48),
          seed, "reg");
    check(test_corpus::generate_memory_program(seed, /*body_len=*/48), seed,
          "mem");
  }
}

TEST(Rv32Threaded, decodeRejectsLikeCanonical)
{
  EXPECT_EQ(
      ThreadedProgram::create(std::vector<uint8_t>{0x13, 0x00, 0x00}).error(),
      ProgramError::kCodeSizeNotAligned);
  const std::vector<uint8_t> compressed = {0x01, 0x00, 0x00, 0x00};
  EXPECT_EQ(ThreadedProgram::create(compressed).error(),
            ProgramError::kUnsupportedCompressedInstruction);
  std::vector<uint8_t> csr(4);
  store_u32_le(csr, 0, 0x00001073U);
  EXPECT_EQ(ThreadedProgram::create(csr).error(),
            ProgramError::kUnsupportedSystemInstruction);
}

// Smoke test: the threaded target builds and links. Real tests arrive in later
// tasks. Verifies HOGD_MUSTTAIL is defined (compiles) and a tail-call lowers.
[[nodiscard]] int
tail_leaf(int x)
{
  return x + 1;
}
[[nodiscard]] int
tail_caller(int x)
{
  HOGD_MUSTTAIL return tail_leaf(x);
}

TEST(Rv32Threaded, harnessBuildsAndMusttailLowers)
{
  EXPECT_EQ(tail_caller(41), 42);
}

} // namespace
} // namespace hogd::rv32
