#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/arch_trace.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/machine.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/spec_model.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_support.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

// This is an in-tree spec-model cross-check, not an external differential
// oracle. It is designed to catch local VM regressions quickly while
// Spike/sail-riscv or another independent oracle remains an open M5 gate.

// NOLINTBEGIN(google-readability-avoid-underscore-in-googletest-name)

namespace hogd::rv32 {
namespace {

using test_program::branch;
using test_program::i_type;
using test_program::j_type;
using test_program::jalr;
using test_program::load;
using test_program::make_program;
using test_program::op;
using test_program::store;
using test_program::u_type;

struct CrosscheckCase {
  std::string name;
  std::vector<spec::SpecInstruction> instructions;
  std::array<uint32_t, kRegisterCount> initial_regs = {};
  std::vector<uint8_t> initial_memory;
  uint64_t gas_limit = 1'000;
};

StepResult
run_machine(Machine &machine, Host &host)
{
  for (size_t steps = 0; steps < 4096; ++steps) {
    const StepResult result = machine.step();
    switch (result) {
    case StepResult::kContinue:
      continue;
    case StepResult::kEcall:
    {
      const auto pending = machine.pending_ecall();
      if (!pending.has_value()) {
        return StepResult::kTrap;
      }
      const StepResult completed =
          machine.complete_ecall(host.ecall(machine.view(), pending.value()));
      if (completed == StepResult::kContinue) {
        continue;
      }
      return completed;
    }
    case StepResult::kHalted:
    case StepResult::kTrap:
      return result;
    default:
      return StepResult::kTrap;
    }
  }
  return StepResult::kTrap;
}

spec::SpecInstruction
spec(uint32_t word, Op op_value, uint8_t rd = 0, uint8_t rs1 = 0,
     uint8_t rs2 = 0, int32_t imm = 0)
{
  return spec::SpecInstruction{.word = word,
                               .op = op_value,
                               .rd = rd,
                               .rs1 = rs1,
                               .rs2 = rs2,
                               .imm = imm,
                               .gas = gas_cost_for_op(op_value)};
}

spec::SpecInstruction
op_imm(Op op_value, uint8_t rd, uint8_t rs1, int32_t imm)
{
  uint8_t funct3 = 0;
  auto encoded_imm = static_cast<uint32_t>(imm);
  switch (op_value) {
  case Op::kAddi:
    funct3 = 0;
    break;
  case Op::kSlti:
    funct3 = 2;
    break;
  case Op::kSltiu:
    funct3 = 3;
    break;
  case Op::kXori:
    funct3 = 4;
    break;
  case Op::kOri:
    funct3 = 6;
    break;
  case Op::kAndi:
    funct3 = 7;
    break;
  case Op::kSlli:
    funct3 = 1;
    encoded_imm = static_cast<uint32_t>(imm) & 0x1fU;
    break;
  case Op::kSrli:
    funct3 = 5;
    encoded_imm = static_cast<uint32_t>(imm) & 0x1fU;
    break;
  case Op::kSrai:
    funct3 = 5;
    encoded_imm = 0x400U | (static_cast<uint32_t>(imm) & 0x1fU);
    break;
  default:
    break;
  }
  return spec(i_type(0x13U, rd, funct3, rs1, static_cast<int32_t>(encoded_imm)),
              op_value, rd, rs1, 0, imm);
}

spec::SpecInstruction
op_reg(Op op_value, uint8_t rd, uint8_t rs1, uint8_t rs2)
{
  uint8_t funct3 = 0;
  uint8_t funct7 = 0;
  switch (op_value) {
  case Op::kAdd:
    funct3 = 0;
    break;
  case Op::kSub:
    funct3 = 0;
    funct7 = 0x20;
    break;
  case Op::kSll:
    funct3 = 1;
    break;
  case Op::kSlt:
    funct3 = 2;
    break;
  case Op::kSltu:
    funct3 = 3;
    break;
  case Op::kXor:
    funct3 = 4;
    break;
  case Op::kSrl:
    funct3 = 5;
    break;
  case Op::kSra:
    funct3 = 5;
    funct7 = 0x20;
    break;
  case Op::kOr:
    funct3 = 6;
    break;
  case Op::kAnd:
    funct3 = 7;
    break;
  case Op::kMul:
    funct3 = 0;
    funct7 = 1;
    break;
  case Op::kMulh:
    funct3 = 1;
    funct7 = 1;
    break;
  case Op::kMulhsu:
    funct3 = 2;
    funct7 = 1;
    break;
  case Op::kMulhu:
    funct3 = 3;
    funct7 = 1;
    break;
  case Op::kDiv:
    funct3 = 4;
    funct7 = 1;
    break;
  case Op::kDivu:
    funct3 = 5;
    funct7 = 1;
    break;
  case Op::kRem:
    funct3 = 6;
    funct7 = 1;
    break;
  case Op::kRemu:
    funct3 = 7;
    funct7 = 1;
    break;
  default:
    break;
  }
  return spec(op(rd, rs1, rs2, funct3, funct7), op_value, rd, rs1, rs2);
}

uint8_t
random_reg(std::mt19937_64 &rng)
{
  static constexpr std::array<uint8_t, 30> kRegs = {0,  1,  2,  3,  4,  5,
                                                    6,  7,  8,  9,  10, 11,
                                                    12, 13, 14, 15, 16, 17,
                                                    18, 19, 20, 21, 22, 23,
                                                    24, 25, 26, 27, 28, 30};
  return kRegs[static_cast<size_t>(rng() % kRegs.size())];
}

uint32_t
random_u32(std::mt19937_64 &rng)
{
  return static_cast<uint32_t>(rng());
}

uint32_t
random_u32_inclusive(std::mt19937_64 &rng, uint32_t max)
{
  return static_cast<uint32_t>(rng() % (uint64_t{max} + 1));
}

int32_t
random_i_imm(std::mt19937_64 &rng)
{
  return static_cast<int32_t>(random_u32_inclusive(rng, 4095)) - 2048;
}

uint32_t
random_offset(std::mt19937_64 &rng, uint32_t width)
{
  // Any byte offset in [128, 384 - width]: the range the aligned-only version
  // spanned, without rounding to the access width -- unaligned data access is
  // legal and must agree between Machine and the spec model.
  return 128 + random_u32_inclusive(rng, 256 - width);
}

spec::SpecInstruction
random_instruction(std::mt19937_64 &rng, size_t index, size_t count)
{
  static constexpr std::array<Op, 18> kRegOps = {
      Op::kAdd,    Op::kSub,   Op::kSll, Op::kSlt,  Op::kSltu, Op::kXor,
      Op::kSrl,    Op::kSra,   Op::kOr,  Op::kAnd,  Op::kMul,  Op::kMulh,
      Op::kMulhsu, Op::kMulhu, Op::kDiv, Op::kDivu, Op::kRem,  Op::kRemu};
  static constexpr std::array<Op, 9> kImmOps = {Op::kAddi,  Op::kSlti,
                                                Op::kSltiu, Op::kXori,
                                                Op::kOri,   Op::kAndi,
                                                Op::kSlli,  Op::kSrli,
                                                Op::kSrai};
  static constexpr std::array<Op, 6> kBranchOps = {Op::kBeq,  Op::kBne,
                                                   Op::kBlt,  Op::kBge,
                                                   Op::kBltu, Op::kBgeu};

  const uint32_t choice = random_u32_inclusive(rng, 99);
  if (choice < 38) {
    return op_reg(kRegOps[static_cast<size_t>(rng() % kRegOps.size())],
                  random_reg(rng), random_reg(rng), random_reg(rng));
  }
  if (choice < 68) {
    const Op op_value = kImmOps[static_cast<size_t>(rng() % kImmOps.size())];
    const int32_t imm =
        op_value == Op::kSlli || op_value == Op::kSrli || op_value == Op::kSrai
            ? static_cast<int32_t>(random_u32_inclusive(rng, 31))
            : random_i_imm(rng);
    return op_imm(op_value, random_reg(rng), random_reg(rng), imm);
  }
  if (choice < 80) {
    static constexpr std::array<std::pair<Op, uint8_t>, 5> kLoadOps = {{
        {Op::kLb, 0},
        {Op::kLh, 1},
        {Op::kLw, 2},
        {Op::kLbu, 4},
        {Op::kLhu, 5},
    }};
    const auto [op_value, funct3] =
        kLoadOps[static_cast<size_t>(rng() % kLoadOps.size())];
    const uint32_t width = spec::load_width(op_value);
    const auto imm = static_cast<int32_t>(random_offset(rng, width) - 128);
    const uint8_t rd = random_reg(rng);
    return spec(load(rd, funct3, 29, imm), op_value, rd, 29, 0, imm);
  }
  if (choice < 90) {
    static constexpr std::array<std::pair<Op, uint8_t>, 3> kStoreOps = {{
        {Op::kSb, 0},
        {Op::kSh, 1},
        {Op::kSw, 2},
    }};
    const auto [op_value, funct3] =
        kStoreOps[static_cast<size_t>(rng() % kStoreOps.size())];
    const uint32_t width = spec::store_width(op_value);
    const auto imm = static_cast<int32_t>(random_offset(rng, width) - 128);
    const uint8_t rs2 = random_reg(rng);
    return spec(store(29, rs2, imm, funct3), op_value, 0, 29, rs2, imm);
  }
  if (choice < 96 && index + 2 < count) {
    const Op op_value =
        kBranchOps[static_cast<size_t>(rng() % kBranchOps.size())];
    uint8_t funct3 = 0;
    switch (op_value) {
    case Op::kBeq:
      funct3 = 0;
      break;
    case Op::kBne:
      funct3 = 1;
      break;
    case Op::kBlt:
      funct3 = 4;
      break;
    case Op::kBge:
      funct3 = 5;
      break;
    case Op::kBltu:
      funct3 = 6;
      break;
    case Op::kBgeu:
      funct3 = 7;
      break;
    default:
      break;
    }
    const uint8_t rs1 = random_reg(rng);
    const uint8_t rs2 = choice % 2 == 0 ? rs1 : random_reg(rng);
    return spec(branch(funct3, rs1, rs2, 8), op_value, 0, rs1, rs2, 8);
  }
  if (choice < 98 && index + 2 < count) {
    const uint8_t rd = random_reg(rng);
    return spec(j_type(rd, 8), Op::kJal, rd, 0, 0, 8);
  }
  if (choice < 99) {
    const uint8_t rd = random_reg(rng);
    const uint32_t imm = random_u32(rng) & 0xfffff000U;
    return spec(u_type(0x37U, rd, imm), Op::kLui, rd, 0, 0,
                std::bit_cast<int32_t>(imm));
  }
  const uint8_t rd = random_reg(rng);
  const uint32_t imm = random_u32(rng) & 0xfffff000U;
  return spec(u_type(0x17U, rd, imm), Op::kAuipc, rd, 0, 0,
              std::bit_cast<int32_t>(imm));
}

CrosscheckCase
make_random_case(uint64_t seed, size_t instruction_count)
{
  std::mt19937_64 rng(seed);
  CrosscheckCase test_case;
  test_case.name = "seed_" + std::to_string(seed);
  test_case.initial_memory.resize(512);
  for (uint8_t &byte : test_case.initial_memory) {
    byte = static_cast<uint8_t>(rng());
  }
  for (size_t i = 1; i < test_case.initial_regs.size(); ++i) {
    test_case.initial_regs[i] = random_u32(rng);
  }
  test_case.initial_regs[29] = 128;

  test_case.instructions.reserve(instruction_count + 1);
  for (size_t i = 0; i < instruction_count; ++i) {
    test_case.instructions.push_back(
        random_instruction(rng, i, instruction_count));
  }
  test_case.instructions.push_back(spec(0x00000073U, Op::kEcall));
  test_case.gas_limit = (instruction_count * kDivInstructionGas) + 10;
  return test_case;
}

std::vector<CrosscheckCase>
make_random_crosscheck_cases()
{
  std::vector<CrosscheckCase> cases;
  cases.reserve(64);
  for (uint64_t i = 0; i < 64; ++i) {
    cases.push_back(make_random_case(0xbadc0ffee0ddf00dULL + i, 96));
  }
  return cases;
}

std::vector<uint32_t>
words_for(std::span<const spec::SpecInstruction> instructions)
{
  std::vector<uint32_t> words;
  words.reserve(instructions.size());
  for (const spec::SpecInstruction &instruction : instructions) {
    words.push_back(instruction.word);
  }
  return words;
}

void
compare_to_machine(const CrosscheckCase &test_case)
{
  const std::vector<uint32_t> words = words_for(test_case.instructions);
  const auto program =
      make_program(std::span<const uint32_t>(words.data(), words.size()));
  ASSERT_NE(program, nullptr) << test_case.name;

  spec::SpecState expected;
  expected.regs = test_case.initial_regs;
  expected.regs[0] = 0;
  expected.memory = test_case.initial_memory;
  expected.gas_remaining = test_case.gas_limit;
  const StepResult expected_result =
      spec::run_spec(expected, test_case.instructions);

  Machine machine(program,
                  static_cast<uint32_t>(test_case.initial_memory.size()),
                  test_case.gas_limit);
  for (size_t i = 1; i < test_case.initial_regs.size(); ++i) {
    MachineTestPeer::set_reg(machine, i, test_case.initial_regs[i]);
  }
  ASSERT_TRUE(machine.view().write_memory(0, test_case.initial_memory))
      << test_case.name;

  RecordingHost host;
  const StepResult actual_result = run_machine(machine, host);

  EXPECT_EQ(actual_result, expected_result) << test_case.name;

  // Compare the full observable state through the shared differential-testing
  // representation (arch_trace.h) -- the same ArchState an external oracle
  // would produce. halt_code is 0 unless halted on both the spec and the
  // machine, so this unconditional comparison matches the previous
  // result-gated halt_code check.
  const ArchState expected_state = spec::spec_state_to_arch(expected);
  const ArchState actual_state = capture_arch_state(machine);
  EXPECT_TRUE(actual_state == expected_state)
      << test_case.name << ": "
      << describe_divergence(expected_state, actual_state);
}

// The production ThreadedMachine cannot seed registers directly, so the
// initial register file is materialized by a lui/addi prologue executed (and
// charged) identically by the spec model.
std::vector<spec::SpecInstruction>
register_prologue(std::array<uint32_t, kRegisterCount> const &regs)
{
  std::vector<spec::SpecInstruction> prologue;
  for (size_t i = 1; i < regs.size(); ++i) {
    const uint32_t value = regs[i];
    if (value == 0) {
      continue;
    }
    const auto rd = static_cast<uint8_t>(i);
    const uint32_t hi = (value + 0x800U) & 0xfffff000U;
    const auto lo = static_cast<int32_t>(value - hi);
    if (hi != 0) {
      prologue.push_back(spec(u_type(0x37U, rd, hi), Op::kLui, rd, 0, 0,
                              std::bit_cast<int32_t>(hi)));
      if (lo != 0) {
        prologue.push_back(op_imm(Op::kAddi, rd, rd, lo));
      }
    }
    else {
      prologue.push_back(op_imm(Op::kAddi, rd, 0, lo));
    }
  }
  return prologue;
}

void
compare_to_threaded_machine(const CrosscheckCase &test_case)
{
  std::vector<spec::SpecInstruction> instructions =
      register_prologue(test_case.initial_regs);
  const uint64_t gas_limit =
      test_case.gas_limit + (instructions.size() * kBaseInstructionGas);
  instructions.insert(instructions.end(), test_case.instructions.begin(),
                      test_case.instructions.end());

  spec::SpecState expected;
  expected.memory = test_case.initial_memory;
  expected.gas_remaining = gas_limit;
  const StepResult expected_result = spec::run_spec(expected, instructions);

  const std::vector<uint32_t> words = words_for(instructions);
  const std::vector<uint8_t> bytes = test_program::code_from_words(words);
  auto pre = ThreadedMachinePreInstantiation::create(
      bytes, test_case.initial_memory, /*memory_start=*/0,
      static_cast<uint32_t>(test_case.initial_memory.size()), /*trace=*/false);
  ASSERT_TRUE(pre.has_value()) << test_case.name;
  ThreadedMachine machine(*pre, gas_limit);
  RecordingHost host;
  const StepResult actual_result = machine.run(host);

  EXPECT_EQ(actual_result, expected_result) << test_case.name;

  // The consensus contract on a trap is {trap class, gas, halt state}; pc,
  // registers, and memory are frame-local state a trap discards, and the
  // block-metered engine may have executed a speculative suffix the
  // per-instruction spec model never reached. On a clean halt the full state
  // is compared.
  EXPECT_EQ(machine.trap(), expected.trap) << test_case.name;
  EXPECT_EQ(machine.gas_remaining(), expected.gas_remaining) << test_case.name;
  EXPECT_EQ(machine.halted(), expected.halted) << test_case.name;
  if (expected.trap == Trap::kNone) {
    EXPECT_EQ(machine.halt_code(), expected.halt_code) << test_case.name;
    EXPECT_EQ(machine.pc(), expected.pc) << test_case.name;
    for (size_t i = 0; i < expected.regs.size(); ++i) {
      EXPECT_EQ(machine.reg(i), expected.regs[i])
          << test_case.name << " x" << i;
    }
    EXPECT_TRUE(std::ranges::equal(machine.memory(), expected.memory))
        << test_case.name;
  }
}

TEST(Rv32SpecModelCrosscheckTest, randomizedProgramsMatchSpec)
{
  for (const CrosscheckCase &test_case : make_random_crosscheck_cases()) {
    SCOPED_TRACE(test_case.name);
    compare_to_machine(test_case);
  }
}

TEST(Rv32SpecModelCrosscheckTest, randomizedProgramsMatchSpecOnThreadedMachine)
{
  for (const CrosscheckCase &test_case : make_random_crosscheck_cases()) {
    SCOPED_TRACE(test_case.name);
    compare_to_threaded_machine(test_case);
  }
}

TEST(Rv32SpecModelCrosscheckTest, semanticTrapsMatchSpec)
{
  std::array<uint32_t, kRegisterCount> regs = {};
  std::vector<uint8_t> memory(64, 0xa5);

  compare_to_machine(CrosscheckCase{
      .name = "taken_branch_to_bad_pc",
      .instructions = {op_imm(Op::kAddi, 1, 0, 1),
                       spec(branch(0, 1, 1, 12), Op::kBeq, 0, 1, 1, 12),
                       op_imm(Op::kAddi, 2, 0, 99)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  compare_to_machine(CrosscheckCase{
      .name = "jal_to_bad_pc",
      .instructions = {spec(j_type(1, 8), Op::kJal, 1, 0, 0, 8)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  compare_to_machine(CrosscheckCase{
      .name = "jalr_to_bad_pc",
      .instructions = {op_imm(Op::kAddi, 1, 0, 13),
                       spec(jalr(2, 1, 0), Op::kJalr, 2, 1, 0, 0),
                       spec(0x00000073U, Op::kEcall)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  regs[1] = 3;
  compare_to_machine(CrosscheckCase{
      .name = "unaligned_load",
      .instructions = {spec(load(2, 2, 1, 0), Op::kLw, 2, 1, 0, 0),
                       spec(0x00000073U, Op::kEcall)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  regs[1] = 68;
  compare_to_machine(CrosscheckCase{
      .name = "out_of_bounds_store",
      .instructions = {spec(store(1, 2, 0, 2), Op::kSw, 0, 1, 2, 0),
                       spec(0x00000073U, Op::kEcall)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  regs = {};
  compare_to_machine(CrosscheckCase{
      .name = "ebreak_trap",
      .instructions = {op_imm(Op::kAddi, 1, 0, 7),
                       spec(0x00100073U, Op::kEbreak)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  compare_to_machine(CrosscheckCase{
      .name = "unimp_trap",
      .instructions = {op_imm(Op::kAddi, 1, 0, 7),
                       spec(0xc0001073U, Op::kUnimp)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  compare_to_machine(CrosscheckCase{
      .name = "out_of_gas_before_first_mutation",
      .instructions = {op_imm(Op::kAddi, 1, 0, 1),
                       spec(0x00000073U, Op::kEcall)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 0,
  });
}

TEST(Rv32SpecModelCrosscheckTest, semanticTrapsMatchSpecOnThreadedMachine)
{
  std::array<uint32_t, kRegisterCount> regs = {};
  std::vector<uint8_t> memory(64, 0xa5);

  compare_to_threaded_machine(CrosscheckCase{
      .name = "taken_branch_to_bad_pc",
      .instructions = {op_imm(Op::kAddi, 1, 0, 1),
                       spec(branch(0, 1, 1, 12), Op::kBeq, 0, 1, 1, 12),
                       op_imm(Op::kAddi, 2, 0, 99)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  compare_to_threaded_machine(CrosscheckCase{
      .name = "jal_to_bad_pc",
      .instructions = {spec(j_type(1, 8), Op::kJal, 1, 0, 0, 8)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  compare_to_threaded_machine(CrosscheckCase{
      .name = "jalr_to_bad_pc",
      .instructions = {op_imm(Op::kAddi, 1, 0, 13),
                       spec(jalr(2, 1, 0), Op::kJalr, 2, 1, 0, 0),
                       spec(0x00000073U, Op::kEcall)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  regs[1] = 3;
  compare_to_threaded_machine(CrosscheckCase{
      .name = "unaligned_load",
      .instructions = {spec(load(2, 2, 1, 0), Op::kLw, 2, 1, 0, 0),
                       spec(0x00000073U, Op::kEcall)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  regs[1] = 68;
  compare_to_threaded_machine(CrosscheckCase{
      .name = "out_of_bounds_store",
      .instructions = {spec(store(1, 2, 0, 2), Op::kSw, 0, 1, 2, 0),
                       spec(0x00000073U, Op::kEcall)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  regs = {};
  compare_to_threaded_machine(CrosscheckCase{
      .name = "ebreak_trap",
      .instructions = {op_imm(Op::kAddi, 1, 0, 7),
                       spec(0x00100073U, Op::kEbreak)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  compare_to_threaded_machine(CrosscheckCase{
      .name = "unimp_trap",
      .instructions = {op_imm(Op::kAddi, 1, 0, 7),
                       spec(0xc0001073U, Op::kUnimp)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 10,
  });

  compare_to_threaded_machine(CrosscheckCase{
      .name = "out_of_gas_before_first_mutation",
      .instructions = {op_imm(Op::kAddi, 1, 0, 1),
                       spec(0x00000073U, Op::kEcall)},
      .initial_regs = regs,
      .initial_memory = memory,
      .gas_limit = 0,
  });
}

} // namespace
} // namespace hogd::rv32

// NOLINTEND(google-readability-avoid-underscore-in-googletest-name)
