#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "hogd/hogexec/runtime/rv32/test_program_builder.h"

// Deterministic constrained-random RV32IM program generator for differential
// testing. The generated programs are register-only and forward-only by design,
// so the SAME corpus is valid on our Machine, the in-tree spec model, AND an
// external oracle that loads code at a nonzero base with a constant pc offset
// (SpikeOracle). There is no production code here; it is harness
// infrastructure.
//
// Deliberate exclusions (so the constant-offset pc mapping holds and execution
// always terminates):
//   - no loads/stores  -- avoids the separate data-region mapping;
//   - no jalr          -- computed (absolute) jump target;
//   - no auipc, no linking jal (rd != 0) -- these write a pc-relative value
//   into
//     a register, which differs by the oracle's base offset;
//   - control flow is forward-only (branch/jal skip exactly one instruction),
//     so pc advances monotonically toward the terminal ecall.
// This still exercises the full arithmetic/logical/shift/mul/div data path plus
// forward branches -- the bulk of the ISA and where most interpreter
// optimizations and subtle bugs live. auipc/jalr/memory join once the oracle
// uses a 0-based unified memory map.

namespace hogd::rv32::test_corpus {

struct RTypeOp {
  uint8_t funct3;
  uint8_t funct7;
};

// add, sub, sll, slt, sltu, xor, srl, sra, or, and, then mul, mulh, mulhsu,
// mulhu, div, divu, rem, remu.
inline constexpr std::array<RTypeOp, 18> kRegisterOps = {{
    {0, 0x00},
    {0, 0x20},
    {1, 0x00},
    {2, 0x00},
    {3, 0x00},
    {4, 0x00},
    {5, 0x00},
    {5, 0x20},
    {6, 0x00},
    {7, 0x00},
    {0, 0x01},
    {1, 0x01},
    {2, 0x01},
    {3, 0x01},
    {4, 0x01},
    {5, 0x01},
    {6, 0x01},
    {7, 0x01},
}};

// addi, slti, sltiu, xori, ori, andi (the non-shift OP-IMM funct3 values).
inline constexpr std::array<uint8_t, 6> kImmFunct3 = {0, 2, 3, 4, 6, 7};

// beq, bne, blt, bge, bltu, bgeu.
inline constexpr std::array<uint8_t, 6> kBranchFunct3 = {0, 1, 4, 5, 6, 7};

inline uint8_t
random_reg(std::mt19937_64 &rng)
{
  return static_cast<uint8_t>(rng() % 32);
}

inline int32_t
random_imm12(std::mt19937_64 &rng)
{
  return static_cast<int32_t>(rng() % 4096) - 2048;
}

// Generate a register-only program of `body_len` instructions plus a terminal
// ecall, fully determined by `seed`.
inline std::vector<uint32_t>
generate_register_only_program(uint64_t seed, size_t body_len)
{
  std::mt19937_64 rng(seed);
  std::vector<uint32_t> code;
  code.reserve(body_len + 1);

  for (size_t i = 0; i < body_len; ++i) {
    const uint32_t choice = rng() % 100;
    if (choice < 45) {
      const RTypeOp r = kRegisterOps[rng() % kRegisterOps.size()];
      code.push_back(test_program::op(random_reg(rng), random_reg(rng),
                                      random_reg(rng), r.funct3, r.funct7));
    }
    else if (choice < 70) {
      const uint8_t funct3 = kImmFunct3[rng() % kImmFunct3.size()];
      code.push_back(test_program::i_type(0x13U, random_reg(rng), funct3,
                                          random_reg(rng), random_imm12(rng)));
    }
    else if (choice < 82) {
      const uint32_t kind = rng() % 3;
      const auto shamt = static_cast<int32_t>(rng() % 32);
      const uint8_t rd = random_reg(rng);
      const uint8_t rs1 = random_reg(rng);
      if (kind == 0) {
        code.push_back(test_program::i_type(0x13U, rd, 1, rs1, shamt)); // slli
      }
      else if (kind == 1) {
        code.push_back(test_program::i_type(0x13U, rd, 5, rs1, shamt)); // srli
      }
      else {
        code.push_back(test_program::i_type(0x13U, rd, 5, rs1,
                                            0x400 | shamt)); // srai
      }
    }
    else if (choice < 90) {
      const uint32_t imm = static_cast<uint32_t>(rng()) & 0xfffff000U;
      code.push_back(test_program::u_type(0x37U, random_reg(rng), imm)); // lui
    }
    else if (choice < 96 && i + 2 < body_len) {
      // Forward branch that skips exactly one instruction; the guard keeps the
      // target inside the body (before the ecall).
      const uint8_t funct3 = kBranchFunct3[rng() % kBranchFunct3.size()];
      code.push_back(
          test_program::branch(funct3, random_reg(rng), random_reg(rng), 8));
    }
    else if (choice < 98 && i + 2 < body_len) {
      code.push_back(test_program::j_type(0, 8)); // jal x0 (no link), forward
    }
    else {
      code.push_back(test_program::addi(random_reg(rng), random_reg(rng),
                                        random_imm12(rng)));
    }
  }

  code.push_back(0x00000073U); // ecall terminator
  return code;
}

} // namespace hogd::rv32::test_corpus
