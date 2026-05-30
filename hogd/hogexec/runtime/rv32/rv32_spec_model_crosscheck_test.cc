#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/hogexec/runtime/rv32/test_support.h"

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

struct SpecInstruction {
  uint32_t word = 0;
  Op op = Op::kAdd;
  uint8_t rd = 0;
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
  int32_t imm = 0;
  uint64_t gas = kBaseInstructionGas;
};

struct CrosscheckCase {
  std::string name;
  std::vector<SpecInstruction> instructions;
  std::array<uint32_t, kRegisterCount> initial_regs = {};
  std::vector<uint8_t> initial_memory;
  uint64_t gas_limit = 1'000;
};

struct SpecState {
  std::array<uint32_t, kRegisterCount> regs = {};
  std::vector<uint8_t> memory;
  uint32_t pc = 0;
  uint64_t gas_remaining = 0;
  Trap trap = Trap::kNone;
  bool halted = false;
  uint32_t halt_code = 0;
};

uint32_t
bits_from_i32(int32_t value)
{
  return std::bit_cast<uint32_t>(value);
}

int32_t
i32_from_bits(uint32_t value)
{
  return std::bit_cast<int32_t>(value);
}

uint32_t
sign_extend(uint32_t value, uint32_t bits_count)
{
  const uint32_t sign_bit = uint32_t{1} << (bits_count - 1);
  return (value ^ sign_bit) - sign_bit;
}

uint32_t
asr(uint32_t value, uint32_t amount)
{
  if (amount == 0) {
    return value;
  }
  const uint32_t shifted = value >> amount;
  if ((value & 0x80000000U) == 0) {
    return shifted;
  }
  return shifted | (~uint32_t{0} << (32 - amount));
}

uint32_t
mulh(uint32_t lhs, uint32_t rhs)
{
  const int64_t product =
      int64_t{i32_from_bits(lhs)} * int64_t{i32_from_bits(rhs)};
  return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32);
}

uint32_t
mulhsu(uint32_t lhs, uint32_t rhs)
{
  const int64_t product = int64_t{i32_from_bits(lhs)} * int64_t{rhs};
  return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32);
}

uint32_t
mulhu(uint32_t lhs, uint32_t rhs)
{
  const uint64_t product = uint64_t{lhs} * uint64_t{rhs};
  return static_cast<uint32_t>(product >> 32);
}

uint32_t
div_signed(uint32_t lhs, uint32_t rhs)
{
  const int32_t divisor = i32_from_bits(rhs);
  const int32_t dividend = i32_from_bits(lhs);
  if (divisor == 0) {
    return std::numeric_limits<uint32_t>::max();
  }
  if (dividend == std::numeric_limits<int32_t>::min() && divisor == -1) {
    return lhs;
  }
  return bits_from_i32(static_cast<int32_t>(dividend / divisor));
}

uint32_t
rem_signed(uint32_t lhs, uint32_t rhs)
{
  const int32_t divisor = i32_from_bits(rhs);
  const int32_t dividend = i32_from_bits(lhs);
  if (divisor == 0) {
    return lhs;
  }
  if (dividend == std::numeric_limits<int32_t>::min() && divisor == -1) {
    return 0;
  }
  return bits_from_i32(static_cast<int32_t>(dividend % divisor));
}

uint32_t
load_le(std::span<const uint8_t> memory, uint32_t address, uint32_t width)
{
  uint32_t value = 0;
  for (uint32_t i = 0; i < width; ++i) {
    value |= uint32_t{memory[address + i]} << (i * 8);
  }
  return value;
}

void
store_le(std::span<uint8_t> memory, uint32_t address, uint32_t width,
         uint32_t value)
{
  for (uint32_t i = 0; i < width; ++i) {
    memory[address + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void
write_reg(SpecState &state, uint8_t index, uint32_t value)
{
  if (index != 0) {
    state.regs[index] = value;
  }
}

bool
set_trap(SpecState &state, Trap trap)
{
  state.trap = trap;
  state.halted = true;
  return false;
}

bool
validate_control_target(SpecState &state, uint32_t code_bytes, uint32_t target)
{
  if ((target % kInstructionBytes) != 0 || target >= code_bytes) {
    return set_trap(state, Trap::kBadPc);
  }
  return true;
}

bool
check_memory(SpecState &state, uint32_t address, uint32_t width)
{
  if ((address % width) != 0) {
    return set_trap(state, Trap::kMisalignedAccess);
  }
  if (!memory_range_in_bounds(state.memory.size(), address, width)) {
    return set_trap(state, Trap::kBadMemory);
  }
  return true;
}

bool
branch_taken(Op op, uint32_t lhs, uint32_t rhs)
{
  switch (op) {
  case Op::kBeq:
    return lhs == rhs;
  case Op::kBne:
    return lhs != rhs;
  case Op::kBlt:
    return i32_from_bits(lhs) < i32_from_bits(rhs);
  case Op::kBge:
    return i32_from_bits(lhs) >= i32_from_bits(rhs);
  case Op::kBltu:
    return lhs < rhs;
  case Op::kBgeu:
    return lhs >= rhs;
  default:
    return false;
  }
}

uint32_t
load_width(Op op)
{
  if (op == Op::kLb || op == Op::kLbu) {
    return 1;
  }
  if (op == Op::kLh || op == Op::kLhu) {
    return 2;
  }
  return 4;
}

uint32_t
store_width(Op op)
{
  if (op == Op::kSb) {
    return 1;
  }
  if (op == Op::kSh) {
    return 2;
  }
  return 4;
}

StepResult
step_spec(SpecState &state, std::span<const SpecInstruction> instructions)
{
  if (state.halted) {
    return state.trap == Trap::kNone ? StepResult::kHalted : StepResult::kTrap;
  }
  const auto code_bytes =
      static_cast<uint32_t>(instructions.size() * kInstructionBytes);
  if (!validate_control_target(state, code_bytes, state.pc)) {
    return StepResult::kTrap;
  }

  const SpecInstruction &instruction =
      instructions[state.pc / kInstructionBytes];
  if (state.gas_remaining < instruction.gas) {
    set_trap(state, Trap::kOutOfGas);
    return StepResult::kTrap;
  }
  state.gas_remaining -= instruction.gas;

  const uint32_t pc_next = state.pc + kInstructionBytes;
  const uint32_t imm = bits_from_i32(instruction.imm);
  switch (instruction.op) {
  case Op::kLui:
    write_reg(state, instruction.rd, imm);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kAuipc:
    write_reg(state, instruction.rd, state.pc + imm);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kJal:
  {
    const uint32_t target = state.pc + imm;
    if (!validate_control_target(state, code_bytes, target)) {
      return StepResult::kTrap;
    }
    write_reg(state, instruction.rd, pc_next);
    state.pc = target;
    return StepResult::kContinue;
  }
  case Op::kJalr:
  {
    const uint32_t target = (state.regs[instruction.rs1] + imm) & ~uint32_t{1};
    if (!validate_control_target(state, code_bytes, target)) {
      return StepResult::kTrap;
    }
    write_reg(state, instruction.rd, pc_next);
    state.pc = target;
    return StepResult::kContinue;
  }
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
    if (!branch_taken(instruction.op, state.regs[instruction.rs1],
                      state.regs[instruction.rs2])) {
      state.pc = pc_next;
      return StepResult::kContinue;
    }
    if (!validate_control_target(state, code_bytes, state.pc + imm)) {
      return StepResult::kTrap;
    }
    state.pc += imm;
    return StepResult::kContinue;
  case Op::kLb:
  case Op::kLh:
  case Op::kLw:
  case Op::kLbu:
  case Op::kLhu:
  {
    const uint32_t address = state.regs[instruction.rs1] + imm;
    const uint32_t width = load_width(instruction.op);
    if (!check_memory(state, address, width)) {
      return StepResult::kTrap;
    }
    uint32_t value = load_le(state.memory, address, width);
    if (instruction.op == Op::kLb) {
      value = sign_extend(value, 8);
    }
    else if (instruction.op == Op::kLh) {
      value = sign_extend(value, 16);
    }
    write_reg(state, instruction.rd, value);
    state.pc = pc_next;
    return StepResult::kContinue;
  }
  case Op::kSb:
  case Op::kSh:
  case Op::kSw:
  {
    const uint32_t address = state.regs[instruction.rs1] + imm;
    const uint32_t width = store_width(instruction.op);
    if (!check_memory(state, address, width)) {
      return StepResult::kTrap;
    }
    store_le(state.memory, address, width, state.regs[instruction.rs2]);
    state.pc = pc_next;
    return StepResult::kContinue;
  }
  case Op::kAddi:
    write_reg(state, instruction.rd, state.regs[instruction.rs1] + imm);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSlti:
    write_reg(state, instruction.rd,
              i32_from_bits(state.regs[instruction.rs1]) < instruction.imm ? 1
                                                                           : 0);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSltiu:
    write_reg(state, instruction.rd, state.regs[instruction.rs1] < imm ? 1 : 0);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kXori:
    write_reg(state, instruction.rd, state.regs[instruction.rs1] ^ imm);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kOri:
    write_reg(state, instruction.rd, state.regs[instruction.rs1] | imm);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kAndi:
    write_reg(state, instruction.rd, state.regs[instruction.rs1] & imm);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSlli:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] << (imm & 0x1fU));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSrli:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] >> (imm & 0x1fU));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSrai:
    write_reg(state, instruction.rd,
              asr(state.regs[instruction.rs1], imm & 0x1fU));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kAdd:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] + state.regs[instruction.rs2]);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSub:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] - state.regs[instruction.rs2]);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSll:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1]
                  << (state.regs[instruction.rs2] & 0x1fU));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSlt:
    write_reg(state, instruction.rd,
              i32_from_bits(state.regs[instruction.rs1]) <
                      i32_from_bits(state.regs[instruction.rs2])
                  ? 1
                  : 0);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSltu:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] < state.regs[instruction.rs2] ? 1
                                                                        : 0);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kXor:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] ^ state.regs[instruction.rs2]);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSrl:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] >>
                  (state.regs[instruction.rs2] & 0x1fU));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kSra:
    write_reg(state, instruction.rd,
              asr(state.regs[instruction.rs1],
                  state.regs[instruction.rs2] & 0x1fU));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kOr:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] | state.regs[instruction.rs2]);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kAnd:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs1] & state.regs[instruction.rs2]);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kMul:
    write_reg(state, instruction.rd,
              static_cast<uint32_t>(uint64_t{state.regs[instruction.rs1]} *
                                    uint64_t{state.regs[instruction.rs2]}));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kMulh:
    write_reg(state, instruction.rd,
              mulh(state.regs[instruction.rs1], state.regs[instruction.rs2]));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kMulhsu:
    write_reg(state, instruction.rd,
              mulhsu(state.regs[instruction.rs1], state.regs[instruction.rs2]));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kMulhu:
    write_reg(state, instruction.rd,
              mulhu(state.regs[instruction.rs1], state.regs[instruction.rs2]));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kDiv:
    write_reg(state, instruction.rd,
              div_signed(state.regs[instruction.rs1],
                         state.regs[instruction.rs2]));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kDivu:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs2] == 0
                  ? std::numeric_limits<uint32_t>::max()
                  : state.regs[instruction.rs1] / state.regs[instruction.rs2]);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kRem:
    write_reg(state, instruction.rd,
              rem_signed(state.regs[instruction.rs1],
                         state.regs[instruction.rs2]));
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kRemu:
    write_reg(state, instruction.rd,
              state.regs[instruction.rs2] == 0
                  ? state.regs[instruction.rs1]
                  : state.regs[instruction.rs1] % state.regs[instruction.rs2]);
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kFence:
    state.pc = pc_next;
    return StepResult::kContinue;
  case Op::kEcall:
    state.pc = pc_next;
    state.halted = true;
    state.halt_code = state.regs[10];
    return StepResult::kHalted;
  default:
    set_trap(state, Trap::kInvalidInstruction);
    return StepResult::kTrap;
  }
}

StepResult
run_spec(SpecState &state, std::span<const SpecInstruction> instructions)
{
  for (size_t steps = 0; steps < 4096; ++steps) {
    const StepResult result = step_spec(state, instructions);
    if (result != StepResult::kContinue) {
      return result;
    }
  }
  set_trap(state, Trap::kBadPc);
  return StepResult::kTrap;
}

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

SpecInstruction
spec(uint32_t word, Op op_value, uint8_t rd = 0, uint8_t rs1 = 0,
     uint8_t rs2 = 0, int32_t imm = 0)
{
  return SpecInstruction{.word = word,
                         .op = op_value,
                         .rd = rd,
                         .rs1 = rs1,
                         .rs2 = rs2,
                         .imm = imm,
                         .gas = gas_cost_for_op(op_value)};
}

SpecInstruction
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

SpecInstruction
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
aligned_offset(std::mt19937_64 &rng, uint32_t width)
{
  const uint32_t slots = (256 - width) / width;
  return 128 + (width * random_u32_inclusive(rng, slots));
}

SpecInstruction
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
    const uint32_t width = load_width(op_value);
    const auto imm = static_cast<int32_t>(aligned_offset(rng, width) - 128);
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
    const uint32_t width = store_width(op_value);
    const auto imm = static_cast<int32_t>(aligned_offset(rng, width) - 128);
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
words_for(std::span<const SpecInstruction> instructions)
{
  std::vector<uint32_t> words;
  words.reserve(instructions.size());
  for (const SpecInstruction &instruction : instructions) {
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

  SpecState expected;
  expected.regs = test_case.initial_regs;
  expected.regs[0] = 0;
  expected.memory = test_case.initial_memory;
  expected.gas_remaining = test_case.gas_limit;
  const StepResult expected_result = run_spec(expected, test_case.instructions);

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
  const ArchState expected_state{
      .pc = expected.pc,
      .regs = expected.regs,
      .memory = expected.memory,
      .gas_remaining = expected.gas_remaining,
      .trap = expected.trap,
      .halted = expected.halted,
      .halt_code = expected.halt_code,
  };
  const ArchState actual_state = capture_arch_state(machine);
  EXPECT_TRUE(actual_state == expected_state)
      << test_case.name << ": "
      << describe_divergence(expected_state, actual_state);
}

TEST(Rv32SpecModelCrosscheckTest, randomizedProgramsMatchSpec)
{
  for (const CrosscheckCase &test_case : make_random_crosscheck_cases()) {
    SCOPED_TRACE(test_case.name);
    compare_to_machine(test_case);
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
      .name = "misaligned_load",
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
