#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"

// Deterministic constrained-random RV32IM program generators for differential
// testing. Both produce programs valid on our Machine, the in-tree spec model,
// AND the SpikeOracle (which loads code at a nonzero base with a constant pc
// offset). There is no production code here; it is harness infrastructure.
//
// generate_register_only_program: register/control-flow ops, forward-only.
// generate_memory_program: register ops plus loads/stores at any alignment.
//
// Both deliberately exclude the ops the constant-offset pc mapping cannot
// model:
//   - no jalr -- computed (absolute) jump target;
//   - no auipc, no linking jal (rd != 0) -- these write a pc-relative value
//   into
//     a register, which differs by the oracle's base offset (Spike cannot put
//     code at 0; [0, 0x1000) is reserved for devices).
// Together they exercise the full arithmetic/logical/shift/mul/div data path,
// forward branches, and memory access -- the bulk of the ISA and where
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

// Engine-only corpus: not Spike-compatible by design. This exercises the paths
// the direct-threaded engine optimizes but the shared oracle corpus must avoid:
// jalr, auipc+jalr, linking jal, branch-into-fused-tail fallback, bounded
// backward branches, mid-program ecalls, and all current fusion-rule families.
// The RecordingHost should use OnEcall::kContinue; the program terminates by
// falling off the end after the final ecall.
inline std::vector<uint32_t>
generate_engine_only_program(uint64_t seed)
{
  std::mt19937_64 rng(seed);
  std::vector<uint32_t> code;
  code.reserve(96);

  auto pc = [&]() -> uint32_t {
    return static_cast<uint32_t>(code.size() * kInstructionBytes);
  };
  auto small = [&]() -> int32_t {
    return static_cast<int32_t>(rng() % 15) - 7;
  };

  // Scalar setup plus isolated lui+addi, addi+addi, addi+sll, srl+addi, and
  // sll+or fusion candidates. The non-fusible add separators keep the
  // backward-greedy lowerer from consuming an intended tail as the next head.
  code.push_back(
      test_program::addi(1, 0, 5 + static_cast<int32_t>(rng() % 11)));
  code.push_back(test_program::addi(2, 0, 3 + static_cast<int32_t>(rng() % 7)));
  code.push_back(test_program::addi(7, 0, 1));
  code.push_back(test_program::u_type(0x37U, 4, 0x00012000U));
  code.push_back(test_program::addi(4, 4, small()));
  code.push_back(test_program::op(3, 1, 2, 0, 0x00)); // separator add
  code.push_back(test_program::addi(5, 4, small()));
  code.push_back(test_program::addi(5, 5, small()));
  code.push_back(test_program::op(3, 1, 2, 0, 0x00)); // separator add
  code.push_back(test_program::addi(6, 5, 2));
  code.push_back(test_program::op(6, 6, 7, 1, 0x00)); // sll
  code.push_back(test_program::op(3, 1, 2, 0, 0x00)); // separator add
  code.push_back(test_program::op(8, 6, 7, 5, 0x00)); // srl
  code.push_back(test_program::addi(8, 8, small()));
  code.push_back(test_program::op(3, 1, 2, 0, 0x00)); // separator add
  code.push_back(test_program::op(9, 1, 7, 1, 0x00)); // sll
  code.push_back(test_program::op(9, 9, 2, 6, 0x00)); // or

  // Memory fusion candidates. The memory range is deliberately well inside the
  // test's 4096-byte data memory so high-gas runs do not trap mid-run.
  code.push_back(test_program::addi(12, 0, 256));
  code.push_back(
      test_program::addi(13, 0, 40 + static_cast<int32_t>(rng() % 40)));
  code.push_back(test_program::store(12, 13, 0, 2)); // addi+sw
  code.push_back(test_program::store(12, 13, 4, 2)); // sw+addi
  code.push_back(test_program::addi(12, 12, 4));
  code.push_back(test_program::addi(14, 12, -4));
  code.push_back(test_program::load(15, 2, 14, 0)); // addi+lw
  code.push_back(test_program::load(16, 2, 12, 0));
  code.push_back(test_program::op(17, 16, 15, 0, 0x00)); // lw+add
  code.push_back(test_program::load(18, 2, 12, -4));
  code.push_back(test_program::addi(18, 18, small())); // lw+addi
  code.push_back(test_program::load(19, 2, 12, 0));
  code.push_back(test_program::op(20, 15, 19, 0, 0x00)); // lw feeds rs2
  code.push_back(test_program::load(18, 2, 12, -4));
  code.push_back(test_program::store(12, 18, 8, 2)); // lw+sw

  // auipc+jalr fused head path: target is the instruction after one skipped
  // slot. The jalr link register is nonzero to cover link visibility.
  {
    code.push_back(test_program::u_type(0x17U, 19, 0)); // auipc x19,0
    code.push_back(test_program::jalr(20, 19, 12));
    code.push_back(test_program::addi(24, 0, 99)); // skipped
    code.push_back(test_program::addi(24, 0, 1));  // target
  }

  // Branch into the jalr tail of an otherwise fusible auipc+jalr pair. The tail
  // slot must remain a valid standalone handler for this path.
  {
    const uint32_t target = pc() + 20;
    code.push_back(
        test_program::addi(21, 0, static_cast<int32_t>(target - 12)));
    code.push_back(test_program::branch(0, 0, 0, 8)); // to jalr tail
    code.push_back(test_program::u_type(0x17U, 21, 0));
    code.push_back(test_program::jalr(22, 21, 12));
    code.push_back(test_program::addi(25, 0, 99)); // skipped
    code.push_back(test_program::addi(25, 0, 2));  // target
  }

  // Linking jal, then a mid-program ecall that continues through the host.
  code.push_back(test_program::j_type(23, 8));
  code.push_back(test_program::addi(26, 0, 99)); // skipped
  code.push_back(test_program::addi(26, 0, 3));
  code.push_back(test_program::addi(10, 0, static_cast<int32_t>(seed & 31U)));
  code.push_back(0x00000073U); // ecall, host continues with a0=0
  code.push_back(test_program::addi(27, 10, 1));

  // Bounded backward loop. The addi+bne pair is a backward op_alu_branch fusion
  // candidate; the loop count keeps traces well below the runaway guard.
  const int32_t trips = 1 + static_cast<int32_t>(rng() % 5);
  code.push_back(test_program::addi(28, 0, trips));
  code.push_back(test_program::addi(29, 0, 0));
  code.push_back(test_program::addi(29, 29, 1));
  code.push_back(test_program::addi(28, 28, -1));
  code.push_back(test_program::branch(1, 28, 0, -8)); // bne x28,x0,loop

  // A few seed-dependent non-control ops keep the corpus from being one fixed
  // trace while preserving deterministic termination.
  for (size_t i = 0; i < 8; ++i) {
    if ((rng() & 1U) == 0) {
      const RTypeOp r = kRegisterOps[rng() % kRegisterOps.size()];
      code.push_back(test_program::op(random_reg(rng), random_reg(rng),
                                      random_reg(rng), r.funct3, r.funct7));
    }
    else {
      const uint8_t funct3 = kImmFunct3[rng() % kImmFunct3.size()];
      code.push_back(test_program::i_type(0x13U, random_reg(rng), funct3,
                                          random_reg(rng), random_imm12(rng)));
    }
  }

  code.push_back(test_program::addi(10, 0, 0));
  code.push_back(0x00000073U); // ecall, then fall off end under kContinue host
  return code;
}

} // namespace hogd::rv32::test_corpus
