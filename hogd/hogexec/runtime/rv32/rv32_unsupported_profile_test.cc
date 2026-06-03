#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"

namespace hogd::rv32 {
namespace {

struct UnsupportedProfileCase {
  std::string_view upstream_source;
  std::string_view instruction;
  uint32_t word = 0;
  ProgramError expected_error = ProgramError::kUnsupportedInstruction;
};

constexpr std::array<UnsupportedProfileCase, 17> kUnsupportedProfileCases = {{
    {.upstream_source = "rv32ua/lrsc.S",
     .instruction = "lr.w",
     .word = 0x100120afU,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32ua/amoadd_w.S",
     .instruction = "amoadd.w",
     .word = 0x004121afU,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uc/rvc.S",
     .instruction = "c.nop",
     .word = 0x00000001U,
     .expected_error = ProgramError::kUnsupportedCompressedInstruction},
    {.upstream_source = "rv32ui/fence_i.S",
     .instruction = "fence.i",
     .word = 0x0000100fU,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uf/fadd.S",
     .instruction = "fadd.s",
     .word = 0x0020f1d3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32ud/fadd.S",
     .instruction = "fadd.d",
     .word = 0x0220f1d3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uzfh/fadd.S",
     .instruction = "fadd.h",
     .word = 0x0420f1d3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uzbc/clmul.S",
     .instruction = "clmul",
     .word = 0x0a2091b3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uzba/sh1add.S",
     .instruction = "sh1add",
     .word = 0x2020a1b3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uzbb/andn.S",
     .instruction = "andn",
     .word = 0x4020f1b3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uzbkb/pack.S",
     .instruction = "pack",
     .word = 0x0820c1b3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uzbkx/xperm4.S",
     .instruction = "xperm4",
     .word = 0x2820a1b3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uzbs/bset.S",
     .instruction = "bset",
     .word = 0x282091b3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32uzicond/czero_eqz.S",
     .instruction = "czero.eqz",
     .word = 0x0e20d1b3U,
     .expected_error = ProgramError::kUnsupportedInstruction},
    {.upstream_source = "rv32si/wfi.S",
     .instruction = "wfi",
     .word = 0x10500073U,
     .expected_error = ProgramError::kUnsupportedSystemInstruction},
    {.upstream_source = "rv32si/csr.S",
     .instruction = "csrrw",
     .word = 0x001110f3U,
     .expected_error = ProgramError::kUnsupportedSystemInstruction},
    {.upstream_source = "rv32mi/illegal.S",
     .instruction = "mret",
     .word = 0x30200073U,
     .expected_error = ProgramError::kUnsupportedSystemInstruction},
}};

std::vector<uint8_t>
code_from_word(uint32_t word)
{
  return test_program::code_from_words({word});
}

TEST(Rv32UnsupportedProfileTest,
     rejectsUpstreamInspiredEncodingsAtProgramBoundary)
{
  for (const UnsupportedProfileCase &test_case : kUnsupportedProfileCases) {
    SCOPED_TRACE(testing::Message()
                 << test_case.upstream_source << " " << test_case.instruction);
    const std::vector<uint8_t> code = code_from_word(test_case.word);

    const ProgramResult validation =
        Program::validate(std::span<const uint8_t>(code.data(), code.size()));
    EXPECT_FALSE(validation.ok);
    EXPECT_EQ(validation.error, test_case.expected_error);

    auto program =
        Program::create(std::span<const uint8_t>(code.data(), code.size()));
    ASSERT_FALSE(program.has_value());
    EXPECT_EQ(program.error(), test_case.expected_error);
  }
}

TEST(Rv32UnsupportedProfileTest,
     rejectsUpstreamInspiredEncodingsAtArtifactBoundary)
{
  const std::array<uint8_t, 16> initial_memory = {};

  for (const UnsupportedProfileCase &test_case : kUnsupportedProfileCases) {
    SCOPED_TRACE(testing::Message()
                 << test_case.upstream_source << " " << test_case.instruction);
    const std::vector<uint32_t> words = {test_case.word};
    const std::vector<uint8_t> artifact = test_artifact::build_artifact(
        std::span<const uint32_t>(words.data(), words.size()), initial_memory);

    auto parsed = parse_artifact(
        std::span<const uint8_t>(artifact.data(), artifact.size()));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), ArtifactError::kInvalidProgram);
  }
}

} // namespace
} // namespace hogd::rv32
