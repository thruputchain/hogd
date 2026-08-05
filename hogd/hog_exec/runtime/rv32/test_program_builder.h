#pragma once

#include <cstdint>
#include <cstdlib>
#include <expected>
#include <initializer_list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/program.h"

namespace hogd::rv32::test_program {

inline void
require_valid_field(bool condition)
{
  if (!condition) {
    std::abort();
  }
}

inline void
append_u32(std::vector<uint8_t> &code, uint32_t word)
{
  code.push_back(static_cast<uint8_t>(word));
  code.push_back(static_cast<uint8_t>(word >> 8));
  code.push_back(static_cast<uint8_t>(word >> 16));
  code.push_back(static_cast<uint8_t>(word >> 24));
}

inline std::vector<uint8_t>
code_from_words(std::span<const uint32_t> words)
{
  std::vector<uint8_t> code;
  code.reserve(words.size() * kInstructionBytes);
  for (const uint32_t word : words) {
    append_u32(code, word);
  }
  return code;
}

inline std::vector<uint8_t>
code_from_words(std::initializer_list<uint32_t> words)
{
  return code_from_words(
      std::span<const uint32_t>(words.begin(), words.size()));
}

inline std::expected<Program, ProgramError>
create_program(std::span<const uint32_t> words)
{
  const std::vector<uint8_t> code = code_from_words(words);
  return Program::create(std::span<const uint8_t>(code.data(), code.size()));
}

inline std::shared_ptr<const Program>
make_program(std::span<const uint32_t> words)
{
  auto program = create_program(words);
  if (!program.has_value()) {
    return {};
  }
  return std::make_shared<Program>(std::move(*program));
}

inline std::shared_ptr<const Program>
make_program(std::initializer_list<uint32_t> words)
{
  return make_program(std::span<const uint32_t>(words.begin(), words.size()));
}

inline uint32_t
r_type(uint8_t funct7, uint8_t rs2, uint8_t rs1, uint8_t funct3, uint8_t rd)
{
  require_valid_field(funct7 < 0x80);
  require_valid_field(rs2 < kRegisterCount);
  require_valid_field(rs1 < kRegisterCount);
  require_valid_field(funct3 < 8);
  require_valid_field(rd < kRegisterCount);
  return uint32_t{funct7} << 25 | uint32_t{rs2} << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | uint32_t{rd} << 7 | 0x33U;
}

inline uint32_t
i_type(uint32_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm)
{
  require_valid_field((opcode & ~0x7fU) == 0);
  require_valid_field(rd < kRegisterCount);
  require_valid_field(funct3 < 8);
  require_valid_field(rs1 < kRegisterCount);
  return (static_cast<uint32_t>(imm) & 0xfffU) << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | uint32_t{rd} << 7 | opcode;
}

inline uint32_t
s_type(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  require_valid_field(funct3 < 8);
  require_valid_field(rs1 < kRegisterCount);
  require_valid_field(rs2 < kRegisterCount);
  const uint32_t uimm = static_cast<uint32_t>(imm) & 0xfffU;
  return (uimm >> 5) << 25 | uint32_t{rs2} << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | (uimm & 0x1fU) << 7 | 0x23U;
}

inline uint32_t
b_type(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  require_valid_field(funct3 < 8);
  require_valid_field(rs1 < kRegisterCount);
  require_valid_field(rs2 < kRegisterCount);
  require_valid_field((imm & 0x1) == 0);
  const uint32_t uimm = static_cast<uint32_t>(imm) & 0x1fffU;
  return ((uimm >> 12) & 0x1U) << 31 | ((uimm >> 5) & 0x3fU) << 25 |
         uint32_t{rs2} << 20 | uint32_t{rs1} << 15 | uint32_t{funct3} << 12 |
         ((uimm >> 1) & 0xfU) << 8 | ((uimm >> 11) & 0x1U) << 7 | 0x63U;
}

inline uint32_t
u_type(uint32_t opcode, uint8_t rd, uint32_t imm)
{
  require_valid_field((opcode & ~0x7fU) == 0);
  require_valid_field(rd < kRegisterCount);
  return (imm & 0xfffff000U) | uint32_t{rd} << 7 | opcode;
}

inline uint32_t
j_type(uint8_t rd, int32_t imm)
{
  require_valid_field(rd < kRegisterCount);
  require_valid_field((imm & 0x1) == 0);
  const uint32_t uimm = static_cast<uint32_t>(imm) & 0x1fffffU;
  return ((uimm >> 20) & 0x1U) << 31 | ((uimm >> 1) & 0x3ffU) << 21 |
         ((uimm >> 11) & 0x1U) << 20 | ((uimm >> 12) & 0xffU) << 12 |
         uint32_t{rd} << 7 | 0x6fU;
}

inline uint32_t
addi(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x13U, rd, 0, rs1, imm);
}

inline uint32_t
load(uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm)
{
  return i_type(0x03U, rd, funct3, rs1, imm);
}

inline uint32_t
jalr(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x67U, rd, 0, rs1, imm);
}

inline uint32_t
op(uint8_t rd, uint8_t rs1, uint8_t rs2, uint8_t funct3, uint8_t funct7)
{
  return r_type(funct7, rs2, rs1, funct3, rd);
}

inline uint32_t
branch(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  return b_type(funct3, rs1, rs2, imm);
}

inline uint32_t
store(uint8_t rs1, uint8_t rs2, int32_t imm, uint8_t funct3)
{
  return s_type(funct3, rs1, rs2, imm);
}

} // namespace hogd::rv32::test_program
