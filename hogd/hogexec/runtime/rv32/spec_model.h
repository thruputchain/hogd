#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/program.h"

// Independent, test-only reference model of the RV32IM core. This is a second
// implementation of the ISA semantics, written by hand from the spec and kept
// deliberately separate from Machine, so the two can be cross-checked
// (rv32_spec_model_crosscheck_test) and so the model can stand in as the first
// Oracle (oracle.h) before an external reference model (Spike/sail-riscv) is
// wired in. There is no production code here; it is harness infrastructure.
//
// The model executes a pre-decoded instruction stream (SpecInstruction). The
// crosscheck builds those instructions by hand -- independent of
// decode_instruction -- so it keeps exercising our decoder. The Oracle path
// feeds it decode_instruction output via decoded_to_spec, matching how an
// external oracle consumes raw words; that path shares our decoder, which is
// acceptable precisely because decode independence is what the external oracle
// adds on top.

namespace hogd::rv32 {

struct SpecInstruction {
  uint32_t word = 0;
  Op op = Op::kAdd;
  uint8_t rd = 0;
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
  int32_t imm = 0;
  uint64_t gas = kBaseInstructionGas;
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

inline uint32_t
bits_from_i32(int32_t value)
{
  return std::bit_cast<uint32_t>(value);
}

inline int32_t
i32_from_bits(uint32_t value)
{
  return std::bit_cast<int32_t>(value);
}

inline uint32_t
sign_extend(uint32_t value, uint32_t bits_count)
{
  const uint32_t sign_bit = uint32_t{1} << (bits_count - 1);
  return (value ^ sign_bit) - sign_bit;
}

inline uint32_t
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

inline uint32_t
mulh(uint32_t lhs, uint32_t rhs)
{
  const int64_t product =
      int64_t{i32_from_bits(lhs)} * int64_t{i32_from_bits(rhs)};
  return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32);
}

inline uint32_t
mulhsu(uint32_t lhs, uint32_t rhs)
{
  const int64_t product = int64_t{i32_from_bits(lhs)} * int64_t{rhs};
  return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32);
}

inline uint32_t
mulhu(uint32_t lhs, uint32_t rhs)
{
  const uint64_t product = uint64_t{lhs} * uint64_t{rhs};
  return static_cast<uint32_t>(product >> 32);
}

inline uint32_t
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

inline uint32_t
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

inline uint32_t
load_le(std::span<const uint8_t> memory, uint32_t address, uint32_t width)
{
  uint32_t value = 0;
  for (uint32_t i = 0; i < width; ++i) {
    value |= uint32_t{memory[address + i]} << (i * 8);
  }
  return value;
}

inline void
store_le(std::span<uint8_t> memory, uint32_t address, uint32_t width,
         uint32_t value)
{
  for (uint32_t i = 0; i < width; ++i) {
    memory[address + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

inline void
write_reg(SpecState &state, uint8_t index, uint32_t value)
{
  if (index != 0) {
    state.regs[index] = value;
  }
}

inline bool
set_trap(SpecState &state, Trap trap)
{
  state.trap = trap;
  state.halted = true;
  return false;
}

inline bool
validate_control_target(SpecState &state, uint32_t code_bytes, uint32_t target)
{
  if ((target % kInstructionBytes) != 0 || target >= code_bytes) {
    return set_trap(state, Trap::kBadPc);
  }
  return true;
}

inline bool
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

inline bool
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

inline uint32_t
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

inline uint32_t
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

inline StepResult
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

inline StepResult
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

// Project a SpecState onto the shared differential-testing representation, so a
// spec run and a Machine run are diffed through one ArchState type.
inline ArchState
spec_state_to_arch(const SpecState &state)
{
  return ArchState{
      .pc = state.pc,
      .regs = state.regs,
      .memory = state.memory,
      .gas_remaining = state.gas_remaining,
      .trap = state.trap,
      .halted = state.halted,
      .halt_code = state.halt_code,
  };
}

// Convert one decoder output into the model's pre-decoded form. This is the
// Oracle entry point: it shares decode_instruction with Machine, so an
// Oracle-vs-Machine run isolates execution-semantics divergence (decode
// independence is what the external oracle adds).
inline SpecInstruction
decoded_to_spec(const DecodedInstruction &decoded)
{
  return SpecInstruction{
      .word = decoded.word,
      .op = decoded.op,
      .rd = decoded.rd,
      .rs1 = decoded.rs1,
      .rs2 = decoded.rs2,
      .imm = decoded.immediate,
      .gas = decoded.gas_cost,
  };
}

// Per-step trace of a spec run, mirroring trace_execution for Machine: capture
// the architectural state after each retired instruction, stopping at halt/trap
// or recording a trailing kTrap on truncation. This is the spec side of a
// lockstep oracle comparison.
inline ExecTrace
trace_spec(SpecState &state, std::span<const SpecInstruction> instructions,
           size_t max_steps = 4096)
{
  ExecTrace trace;
  for (size_t i = 0; i < max_steps; ++i) {
    const ArchState state_before = spec_state_to_arch(state);
    const StepResult result = step_spec(state, instructions);
    trace.steps.push_back(TraceStep{.result = result,
                                    .state_before = state_before,
                                    .state = spec_state_to_arch(state)});
    if (result != StepResult::kContinue) {
      trace.final_result = result;
      return trace;
    }
  }
  trace.final_result = StepResult::kTrap; // Truncated at max_steps.
  return trace;
}

} // namespace hogd::rv32
