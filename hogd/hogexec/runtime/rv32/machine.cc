#include "hogd/hogexec/runtime/rv32/machine.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/isa_ops.h"
#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

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
  // Width is always one of RV32 byte/halfword/word; spell out little-endian
  // accesses so hot load/store paths do not pay a variable-width byte loop.
  switch (width) {
  case 1:
    return memory_[address];
  case 2:
    return uint32_t{memory_[address]} | uint32_t{memory_[address + 1]} << 8;
  case 4:
    return uint32_t{memory_[address]} | uint32_t{memory_[address + 1]} << 8 |
           uint32_t{memory_[address + 2]} << 16 |
           uint32_t{memory_[address + 3]} << 24;
  default:
    assert(false && "invalid RV32 load width");
    std::abort();
  }
}

void
Machine::store(uint32_t address, uint32_t width, uint32_t value)
{
  switch (width) {
  case 1:
    memory_[address] = static_cast<uint8_t>(value);
    return;
  case 2:
    memory_[address] = static_cast<uint8_t>(value);
    memory_[address + 1] = static_cast<uint8_t>(value >> 8);
    return;
  case 4:
    memory_[address] = static_cast<uint8_t>(value);
    memory_[address + 1] = static_cast<uint8_t>(value >> 8);
    memory_[address + 2] = static_cast<uint8_t>(value >> 16);
    memory_[address + 3] = static_cast<uint8_t>(value >> 24);
    return;
  default:
    assert(false && "invalid RV32 store width");
    std::abort();
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
  // Misuse to complete an ecall that was never pending: programmer error.
  assert(pending_ecall_.has_value());
  pending_ecall_.reset();

  // ECALL instruction gas was charged by step(). Debit host gas before
  // committing so an over-budget syscall cannot mutate a0/halt/trap state.
  // consume_gas sets Trap::kOutOfGas on failure.
  if (!consume_gas(result.gas_consumed)) {
    return StepResult::kTrap;
  }

  switch (result.status) {
  case HostStatus::kContinue:
    // Host memory writes happened during Host::ecall through MachineView; the
    // only VM register mutation on a continuing ecall is the returned a0.
    write_reg(10, result.continue_value());
    return StepResult::kContinue;
  case HostStatus::kHalt:
    halt_code_ = result.halt_code();
    halted_ = true;
    return StepResult::kHalted;
  case HostStatus::kOutOfGas:
    // Host OOG keeps ECALL + reported host gas charged and pc advanced; host
    // memory writes persist (no per-ecall rollback).
    set_trap(Trap::kOutOfGas);
    return StepResult::kTrap;
  case HostStatus::kTrap:
    set_host_trap(result.trap_payload());
    return StepResult::kTrap;
  default:
    assert(false && "invalid host status");
    set_host_trap(HostTrapPayload{});
    return StepResult::kTrap;
  }
}

StepResult
Machine::start()
{
  // start() begins a fresh drive; an outstanding ecall must be completed via
  // resume_from_ecall() first. Mirrors ThreadedMachine::start()'s precondition.
  assert(!pending_ecall_.has_value());
  // Drive from the current pc to the next yield. step() returns kEcall when an
  // ecall is pending, so calling start() with an outstanding ecall re-yields
  // kEcall rather than mis-stepping.
  StepResult result = step();
  while (result == StepResult::kContinue) {
    result = step();
  }
  return result;
}

StepResult
Machine::resume_from_ecall(const HostResult &result)
{
  const StepResult applied = complete_ecall(result);
  return applied == StepResult::kContinue ? start() : applied;
}

StepResult
Machine::run(Host &host)
{
  StepResult result = start();
  while (result == StepResult::kEcall) {
    assert(pending_ecall_.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    result = resume_from_ecall(host.ecall(view(), pending_ecall_.value()));
    // NOLINTEND(bugprone-unchecked-optional-access)
  }
  return result;
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
    // Advance PC and stash args before yielding to the host. complete_ecall
    // owns post-host register/trap mutation for both Machine::run and manual
    // step/complete callers; host memory writes are not rolled back (the
    // runtime layer owns transaction-level abort).
    pc_ = pc_next;
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
  case Op::kEbreak:
    set_trap(Trap::kBreakpoint);
    return StepResult::kTrap;
  default:
    assert(false && "invalid decoded op");
    set_trap(Trap::kInvalidInstruction);
    return StepResult::kTrap;
  }
}

} // namespace hogd::rv32
