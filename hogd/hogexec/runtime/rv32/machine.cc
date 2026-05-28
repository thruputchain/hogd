#include "hogd/hogexec/runtime/rv32/machine.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {
namespace {

// Keep architectural values as raw 32-bit patterns. Converting through signed
// C++ arithmetic is only done in helpers that pin the RISC-V edge cases.
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
arithmetic_shift_right(uint32_t value, uint32_t amount)
{
  // C++ signed right shift is implementation-defined, and shifting by 32 is
  // undefined. Decode/execution masks amounts before this helper; amount==0
  // still needs a separate path to avoid the 32-bit fill expression below.
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
  // MULH variants differ only in signedness of the high half. Keep the casts
  // explicit so future refactors do not accidentally collapse the variants.
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
  // RISC-V defines division by zero and signed overflow results; avoid C++ UB.
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
  // RISC-V defines division by zero and signed overflow results; avoid C++ UB.
  if (divisor == 0) {
    return lhs;
  }
  if (dividend == std::numeric_limits<int32_t>::min() && divisor == -1) {
    return 0;
  }
  return bits_from_i32(static_cast<int32_t>(dividend % divisor));
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

} // namespace

Machine::Machine(std::shared_ptr<const Program> program, uint32_t memory_bytes,
                 uint64_t gas, uint32_t initial_pc)
  : program_(std::move(program)), initial_pc_(initial_pc)
{
  if (!program_) {
    construction_trap_ = Trap::kInvalidInstruction;
    set_trap(Trap::kInvalidInstruction);
    return;
  }
  if (!program_->contains_pc(initial_pc_)) {
    construction_trap_ = Trap::kBadPc;
    set_trap(Trap::kBadPc);
    return;
  }
  if (memory_bytes > kMaxMemoryBytes) {
    construction_trap_ = Trap::kBadMemory;
    set_trap(Trap::kBadMemory);
    return;
  }

  memory_.resize(memory_bytes);
  reset(gas);
}

void
Machine::reset(uint64_t gas)
{
  registers_.fill(0);
  std::ranges::fill(memory_, 0);
  pc_ = initial_pc_;
  gas_remaining_ = gas;
  trap_ = Trap::kNone;
  host_trap_payload_.reset();
  halted_ = false;
  halt_code_ = 0;
  pending_ecall_.reset();
  ecall_memory_snapshot_.clear();
  if (construction_trap_ != Trap::kNone) {
    // A machine that failed construction must remain trapped after reset; do
    // not allow benchmark/test reset paths to re-enter execution with no valid
    // program or memory image.
    trap_ = construction_trap_;
    halted_ = true;
    return;
  }
  registers_[2] = static_cast<uint32_t>(memory_.size()) & ~uint32_t{0x0f};
}

MachineView
Machine::view()
{
  return MachineView(pc_, gas_remaining_,
                     std::span<const uint32_t, kRegisterCount>(registers_),
                     memory_);
}

bool
Machine::consume_gas(uint64_t amount)
{
  if (gas_remaining_ < amount) {
    set_trap(Trap::kOutOfGas);
    return false;
  }
  gas_remaining_ -= amount;
  return true;
}

void
Machine::set_trap(Trap trap)
{
  trap_ = trap;
  if (trap != Trap::kHostTrap) {
    host_trap_payload_.reset();
  }
  halted_ = true;
  pending_ecall_.reset();
}

void
Machine::set_host_trap(HostTrapPayload payload)
{
  host_trap_payload_ = payload;
  set_trap(Trap::kHostTrap);
}

void
Machine::write_reg(uint8_t index, uint32_t value)
{
  // RISC-V x0 is hardwired to zero. Centralize the rule so instruction bodies
  // can freely write rd without per-op special cases.
  if (index != 0) {
    registers_[index] = value;
  }
}

bool
Machine::validate_control_target(uint32_t target)
{
  // Code memory is immutable and separate from guest data memory. Every dynamic
  // target is rechecked so computed control flow cannot escape the decoded
  // Program image.
  if (!program_->contains_pc(target)) {
    set_trap(Trap::kBadPc);
    return false;
  }
  return true;
}

bool
Machine::check_load(uint32_t address, uint32_t width)
{
  // Trap class is observable. Alignment wins over bounds, and both checks run
  // after gas debit but before register or memory mutation.
  if ((address % width) != 0) {
    set_trap(Trap::kMisalignedAccess);
    return false;
  }
  if (!memory_range_in_bounds(memory_.size(), address, width)) {
    set_trap(Trap::kBadMemory);
    return false;
  }
  return true;
}

bool
Machine::check_store(uint32_t address, uint32_t width)
{
  // Stores share the load ordering: debit first in step(), then validate all
  // store preconditions, then mutate memory only on success.
  if ((address % width) != 0) {
    set_trap(Trap::kMisalignedAccess);
    return false;
  }
  if (!memory_range_in_bounds(memory_.size(), address, width)) {
    set_trap(Trap::kBadMemory);
    return false;
  }
  return true;
}

uint32_t
Machine::load(uint32_t address, uint32_t width) const
{
  uint32_t value = 0;
  for (uint32_t i = 0; i < width; ++i) {
    value |= uint32_t{memory_[address + i]} << (i * 8);
  }
  return value;
}

void
Machine::store(uint32_t address, uint32_t width, uint32_t value)
{
  for (uint32_t i = 0; i < width; ++i) {
    memory_[address + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

StepResult
Machine::step()
{
  if (pending_ecall_.has_value()) {
    return StepResult::kEcall;
  }
  if (halted_) {
    return trap_ == Trap::kNone ? StepResult::kHalted : StepResult::kTrap;
  }
  if (!program_->contains_pc(pc_)) {
    set_trap(Trap::kBadPc);
    return StepResult::kTrap;
  }

  const DecodedInstruction &instruction =
      program_->instruction_at_aligned_pc(pc_);
  // Consensus ordering: invalid fetch traps without gas; a valid fetched
  // instruction pays gas before any semantic trap or state mutation. This makes
  // OutOfGas win cleanly with no partial instruction effects.
  if (!consume_gas(instruction.gas_cost)) {
    return StepResult::kTrap;
  }

  return execute(instruction);
}

StepResult
Machine::complete_ecall(const HostResult &result)
{
  if (!pending_ecall_.has_value()) {
    set_host_trap(HostTrapPayload{});
    return StepResult::kTrap;
  }
  pending_ecall_.reset();

  // ECALL instruction gas has already been charged by step(). Debit host gas
  // before committing the host result so an over-budget syscall cannot mutate
  // a0, halt state, or final trap state.
  if (!consume_gas(result.gas_consumed)) {
    memory_ = ecall_memory_snapshot_;
    ecall_memory_snapshot_.clear();
    set_trap(Trap::kOutOfGas);
    return StepResult::kTrap;
  }

  switch (result.status) {
  case HostStatus::kContinue:
    // Host memory writes happen during Host::ecall through MachineView. The
    // only VM register mutation on a continuing ecall is the returned a0 value.
    ecall_memory_snapshot_.clear();
    write_reg(10, result.continue_value());
    return StepResult::kContinue;
  case HostStatus::kHalt:
    ecall_memory_snapshot_.clear();
    halt_code_ = result.halt_code();
    halted_ = true;
    return StepResult::kHalted;
  case HostStatus::kOutOfGas:
    // Host OOG rolls back host memory effects. ECALL and reported host gas stay
    // charged, and pc remains advanced past the ECALL.
    memory_ = ecall_memory_snapshot_;
    ecall_memory_snapshot_.clear();
    set_trap(Trap::kOutOfGas);
    return StepResult::kTrap;
  case HostStatus::kTrap:
    ecall_memory_snapshot_.clear();
    set_host_trap(result.trap_payload());
    return StepResult::kTrap;
  default:
    assert(false && "invalid host status");
    ecall_memory_snapshot_.clear();
    set_host_trap(HostTrapPayload{});
    return StepResult::kTrap;
  }
}

StepResult
Machine::run(Host &host)
{
  for (;;) {
    const StepResult result = step();
    switch (result) {
    case StepResult::kContinue:
      continue;
    case StepResult::kEcall:
    {
      if (!pending_ecall_.has_value()) {
        set_host_trap(HostTrapPayload{});
        return StepResult::kTrap;
      }
      const EcallArgs pending_ecall = pending_ecall_.value();
      const HostResult host_result = host.ecall(view(), pending_ecall);
      const StepResult completed = complete_ecall(host_result);
      if (completed == StepResult::kContinue) {
        continue;
      }
      return completed;
    }
    case StepResult::kHalted:
    case StepResult::kTrap:
      return result;
    default:
      assert(false && "invalid step result");
      set_host_trap(HostTrapPayload{});
      return StepResult::kTrap;
    }
  }
}

StepResult
Machine::execute(const DecodedInstruction &instruction)
{
  const uint32_t pc_next = pc_ + kInstructionBytes;
  const uint32_t imm = bits_from_i32(instruction.immediate);

  switch (instruction.op) {
  case Op::kLui:
    write_reg(instruction.rd, imm);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kAuipc:
    write_reg(instruction.rd, pc_ + imm);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kJal:
  {
    const uint32_t target = pc_ + imm;
    if (!validate_control_target(target)) {
      return StepResult::kTrap;
    }
    write_reg(instruction.rd, pc_next);
    pc_ = target;
    return StepResult::kContinue;
  }
  case Op::kJalr:
  {
    // JALR clears bit 0 after adding the immediate; validate the masked target
    // before writing rd so bad targets do not expose partial link writes.
    const uint32_t target = (registers_[instruction.rs1] + imm) & ~uint32_t{1};
    if (!validate_control_target(target)) {
      return StepResult::kTrap;
    }
    write_reg(instruction.rd, pc_next);
    pc_ = target;
    return StepResult::kContinue;
  }
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
  {
    if (!branch_taken(instruction.op, registers_[instruction.rs1],
                      registers_[instruction.rs2])) {
      pc_ = pc_next;
      return StepResult::kContinue;
    }
    const uint32_t target = pc_ + imm;
    if (!validate_control_target(target)) {
      return StepResult::kTrap;
    }
    pc_ = target;
    return StepResult::kContinue;
  }
  case Op::kLb:
  {
    const uint32_t address = registers_[instruction.rs1] + imm;
    if (!check_load(address, 1)) {
      return StepResult::kTrap;
    }
    write_reg(instruction.rd, sign_extend(load(address, 1), 8));
    pc_ = pc_next;
    return StepResult::kContinue;
  }
  case Op::kLh:
  {
    const uint32_t address = registers_[instruction.rs1] + imm;
    if (!check_load(address, 2)) {
      return StepResult::kTrap;
    }
    write_reg(instruction.rd, sign_extend(load(address, 2), 16));
    pc_ = pc_next;
    return StepResult::kContinue;
  }
  case Op::kLw:
  {
    const uint32_t address = registers_[instruction.rs1] + imm;
    if (!check_load(address, 4)) {
      return StepResult::kTrap;
    }
    write_reg(instruction.rd, load(address, 4));
    pc_ = pc_next;
    return StepResult::kContinue;
  }
  case Op::kLbu:
  {
    const uint32_t address = registers_[instruction.rs1] + imm;
    if (!check_load(address, 1)) {
      return StepResult::kTrap;
    }
    write_reg(instruction.rd, load(address, 1));
    pc_ = pc_next;
    return StepResult::kContinue;
  }
  case Op::kLhu:
  {
    const uint32_t address = registers_[instruction.rs1] + imm;
    if (!check_load(address, 2)) {
      return StepResult::kTrap;
    }
    write_reg(instruction.rd, load(address, 2));
    pc_ = pc_next;
    return StepResult::kContinue;
  }
  case Op::kSb:
  {
    const uint32_t address = registers_[instruction.rs1] + imm;
    if (!check_store(address, 1)) {
      return StepResult::kTrap;
    }
    store(address, 1, registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  }
  case Op::kSh:
  {
    const uint32_t address = registers_[instruction.rs1] + imm;
    if (!check_store(address, 2)) {
      return StepResult::kTrap;
    }
    store(address, 2, registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  }
  case Op::kSw:
  {
    const uint32_t address = registers_[instruction.rs1] + imm;
    if (!check_store(address, 4)) {
      return StepResult::kTrap;
    }
    store(address, 4, registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  }
  case Op::kAddi:
    write_reg(instruction.rd, registers_[instruction.rs1] + imm);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSlti:
    write_reg(instruction.rd,
              i32_from_bits(registers_[instruction.rs1]) < instruction.immediate
                  ? 1
                  : 0);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSltiu:
    write_reg(instruction.rd, registers_[instruction.rs1] < imm ? 1 : 0);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kXori:
    write_reg(instruction.rd, registers_[instruction.rs1] ^ imm);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kOri:
    write_reg(instruction.rd, registers_[instruction.rs1] | imm);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kAndi:
    write_reg(instruction.rd, registers_[instruction.rs1] & imm);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSlli:
    // RV32 masks register and immediate shift amounts to five bits.
    write_reg(instruction.rd, registers_[instruction.rs1] << (imm & 0x1fU));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSrli:
    write_reg(instruction.rd, registers_[instruction.rs1] >> (imm & 0x1fU));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSrai:
    write_reg(instruction.rd,
              arithmetic_shift_right(registers_[instruction.rs1], imm & 0x1fU));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kAdd:
    write_reg(instruction.rd,
              registers_[instruction.rs1] + registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSub:
    write_reg(instruction.rd,
              registers_[instruction.rs1] - registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSll:
    write_reg(instruction.rd, registers_[instruction.rs1]
                                  << (registers_[instruction.rs2] & 0x1fU));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSlt:
    write_reg(instruction.rd, i32_from_bits(registers_[instruction.rs1]) <
                                      i32_from_bits(registers_[instruction.rs2])
                                  ? 1
                                  : 0);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSltu:
    write_reg(instruction.rd,
              registers_[instruction.rs1] < registers_[instruction.rs2] ? 1
                                                                        : 0);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kXor:
    write_reg(instruction.rd,
              registers_[instruction.rs1] ^ registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSrl:
    write_reg(instruction.rd, registers_[instruction.rs1] >>
                                  (registers_[instruction.rs2] & 0x1fU));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kSra:
    write_reg(instruction.rd,
              arithmetic_shift_right(registers_[instruction.rs1],
                                     registers_[instruction.rs2] & 0x1fU));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kOr:
    write_reg(instruction.rd,
              registers_[instruction.rs1] | registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kAnd:
    write_reg(instruction.rd,
              registers_[instruction.rs1] & registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kMul:
    write_reg(instruction.rd,
              static_cast<uint32_t>(uint64_t{registers_[instruction.rs1]} *
                                    uint64_t{registers_[instruction.rs2]}));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kMulh:
    write_reg(instruction.rd,
              mulh(registers_[instruction.rs1], registers_[instruction.rs2]));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kMulhsu:
    write_reg(instruction.rd,
              mulhsu(registers_[instruction.rs1], registers_[instruction.rs2]));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kMulhu:
    write_reg(instruction.rd,
              mulhu(registers_[instruction.rs1], registers_[instruction.rs2]));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kDiv:
    write_reg(instruction.rd, div_signed(registers_[instruction.rs1],
                                         registers_[instruction.rs2]));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kDivu:
    write_reg(instruction.rd,
              registers_[instruction.rs2] == 0
                  ? std::numeric_limits<uint32_t>::max()
                  : registers_[instruction.rs1] / registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kRem:
    write_reg(instruction.rd, rem_signed(registers_[instruction.rs1],
                                         registers_[instruction.rs2]));
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kRemu:
    write_reg(instruction.rd,
              registers_[instruction.rs2] == 0
                  ? registers_[instruction.rs1]
                  : registers_[instruction.rs1] % registers_[instruction.rs2]);
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kFence:
    // Single-threaded v1 has no host-visible memory effect for FENCE.
    pc_ = pc_next;
    return StepResult::kContinue;
  case Op::kEcall:
    // Advance PC and snapshot guest-visible state before yielding to the host.
    // complete_ecall owns post-host mutation, trap, and host-OOG rollback
    // ordering for both Machine::run and manual step/complete callers.
    pc_ = pc_next;
    ecall_memory_snapshot_ = memory_;
    pending_ecall_ = EcallArgs{
        .a0 = registers_[10],
        .a1 = registers_[11],
        .a2 = registers_[12],
        .a3 = registers_[13],
        .a4 = registers_[14],
        .a5 = registers_[15],
        .a6 = registers_[16],
        .a7 = registers_[17],
    };
    return StepResult::kEcall;
  default:
    assert(false && "invalid decoded op");
    set_trap(Trap::kInvalidInstruction);
    return StepResult::kTrap;
  }
}

} // namespace hogd::rv32
