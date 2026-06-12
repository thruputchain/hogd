#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "hogd/hogexec/runtime/rv32/test_program_builder.h"

// Deterministic constrained-random RV32IM program generators for differential
// testing. Both produce programs valid on our Machine, the in-tree spec model,
// AND the SpikeOracle (which loads code at a nonzero base with a constant pc
// offset). There is no production code here; it is harness infrastructure.
//
// generate_register_only_program: register/control-flow ops, forward-only.
// generate_memory_program: register ops plus width-aligned loads/stores.
//
// Both deliberately exclude the ops the constant-offset pc mapping cannot
// model:
//   - no jalr -- computed (absolute) jump target;
//   - no auipc, no linking jal (rd != 0) -- these write a pc-relative value
//   into
//     a register, which differs by the oracle's base offset (Spike cannot put
//     code at 0; [0, 0x1000) is reserved for devices).
// Together they exercise the full arithmetic/logical/shift/mul/div data path,
// forward branches, and aligned memory access -- the bulk of the ISA and where
// most interpreter optimizations and subtle bugs live.

namespace hogd::rv32::test_corpus {

struct RTypeOp {
  uint8_t funct3;
  uint8_t funct7;
};

// add, sub, sll, slt, sltu, xor, srl, sra, or, and, then mul, mulh, mulhsu,
// mulhu, div, divu, rem, remu.
inline constexpr std::array<RTypeOp, 18> kRegisterOps = {{
    {.funct3 = 0, .funct7 = 0x00}, // add
    {.funct3 = 0, .funct7 = 0x20}, // sub
    {.funct3 = 1, .funct7 = 0x00}, // sll
    {.funct3 = 2, .funct7 = 0x00}, // slt
    {.funct3 = 3, .funct7 = 0x00}, // sltu
    {.funct3 = 4, .funct7 = 0x00}, // xor
    {.funct3 = 5, .funct7 = 0x00}, // srl
    {.funct3 = 5, .funct7 = 0x20}, // sra
    {.funct3 = 6, .funct7 = 0x00}, // or
    {.funct3 = 7, .funct7 = 0x00}, // and
    {.funct3 = 0, .funct7 = 0x01}, // mul
    {.funct3 = 1, .funct7 = 0x01}, // mulh
    {.funct3 = 2, .funct7 = 0x01}, // mulhsu
    {.funct3 = 3, .funct7 = 0x01}, // mulhu
    {.funct3 = 4, .funct7 = 0x01}, // div
    {.funct3 = 5, .funct7 = 0x01}, // divu
    {.funct3 = 6, .funct7 = 0x01}, // rem
    {.funct3 = 7, .funct7 = 0x01}, // remu
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

// Data layout for memory programs: a region based at kCorpusDataBase (which
// must match SpikeOracle's kSpikeDataBase, above Spike's [0, 0x1000) device
// window). Accesses stay in [kCorpusDataBase, kCorpusDataBase +
// kCorpusDataSpan); a DifferentialProgram running these must set memory_bytes
// to at least kCorpusMemoryBytes.
inline constexpr uint32_t kCorpusDataBase = 0x1000;
inline constexpr uint32_t kCorpusDataSpan = 0x800;
inline constexpr uint32_t kCorpusMemoryBytes = 0x2000;

// Generate a straight-line program (no branches/jal, so pc advances
// monotonically to the terminal ecall) that mixes register ops with
// loads/stores of any alignment into the data region (unaligned access is
// legal; Spike matches when run with Zicclsm). Each memory op re-establishes
// its base via lui, so register writes never strand the base.
inline std::vector<uint32_t>
generate_memory_program(uint64_t seed, size_t body_len)
{
  std::mt19937_64 rng(seed);
  std::vector<uint32_t> code;

  for (size_t i = 0; i < body_len; ++i) {
    if (rng() % 2 == 0) {
      // Register-only op (no control flow, no pc-relative addressing).
      const uint32_t kind = rng() % 100;
      if (kind < 60) {
        const RTypeOp r = kRegisterOps[rng() % kRegisterOps.size()];
        code.push_back(test_program::op(random_reg(rng), random_reg(rng),
                                        random_reg(rng), r.funct3, r.funct7));
      }
      else {
        const uint8_t funct3 = kImmFunct3[rng() % kImmFunct3.size()];
        code.push_back(test_program::i_type(0x13U, random_reg(rng), funct3,
                                            random_reg(rng),
                                            random_imm12(rng)));
      }
    }
    else {
      // Memory op: establish the base, then a load or store at any alignment.
      const auto base = static_cast<uint8_t>(1 + (rng() % 31)); // x1..x31
      code.push_back(test_program::u_type(0x37U, base, kCorpusDataBase));
      const uint32_t wsel = rng() % 3; // 0=byte, 1=half, 2=word
      const uint32_t width = 1U << wsel;
      const auto offset =
          static_cast<int32_t>(rng() % (kCorpusDataSpan - width + 1));
      if (rng() % 2 == 0) {
        // test_program::store is store(rs1, rs2, imm, funct3).
        code.push_back(test_program::store(base, random_reg(rng), offset,
                                           static_cast<uint8_t>(wsel)));
      }
      else {
        uint8_t funct3 = 2; // lw
        if (wsel == 0) {
          funct3 = (rng() % 2 == 0) ? 0 : 4; // lb / lbu
        }
        else if (wsel == 1) {
          funct3 = (rng() % 2 == 0) ? 1 : 5; // lh / lhu
        }
        code.push_back(
            test_program::load(random_reg(rng), funct3, base, offset));
      }
    }
  }

  code.push_back(0x00000073U); // ecall terminator
  return code;
}

} // namespace hogd::rv32::test_corpus
