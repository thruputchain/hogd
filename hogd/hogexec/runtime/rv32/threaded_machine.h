#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/isa_ops.h"
#include "hogd/hogexec/runtime/rv32/musttail.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/threaded_program.h"

// Direct-threaded, musttail RV32IM interpreter. Mirrors Machine's semantics,
// gas/trap ordering, and ecall protocol exactly -- enforced by the
// threaded-vs-canonical differential test in rv32_threaded_test (added in a
// later step) -- but dispatches by tail-calling a precomputed per-instruction
// handler-pointer array instead of a switch.
namespace hogd::rv32 {

class ThreadedMachine {
public:
  ThreadedMachine(std::shared_ptr<const ThreadedProgram> program,
                  uint32_t memory_bytes, uint64_t gas, uint32_t initial_pc = 0)
    : program_(std::move(program)), initial_pc_(initial_pc)
  {
    if (!program_ || !program_->contains_pc(initial_pc_)) {
      construction_trap_ = Trap::kBadPc;
      trap_ = Trap::kBadPc;
      halted_ = true;
      return;
    }
    if (memory_bytes > kMaxMemoryBytes) {
      construction_trap_ = Trap::kBadMemory;
      trap_ = Trap::kBadMemory;
      halted_ = true;
      return;
    }
    memory_.resize(memory_bytes);
    reset(gas);
  }

  void reset(uint64_t gas)
  {
    regs_.fill(0);
    std::ranges::fill(memory_, uint8_t{0});
    pc_ = initial_pc_;
    gas_remaining_ = gas;
    trap_ = Trap::kNone;
    halted_ = false;
    halt_code_ = 0;
    if (construction_trap_ != Trap::kNone) {
      trap_ = construction_trap_;
      halted_ = true;
      return;
    }
    regs_[2] = static_cast<uint32_t>(memory_.size()) & ~uint32_t{0x0f};
  }

  template<class Obs> StepResult run(Host &host, Obs &obs);

  [[nodiscard]] uint32_t reg(size_t i) const
  {
    return i < regs_.size() ? regs_[i] : 0;
  }
  [[nodiscard]] uint32_t pc() const { return pc_; }
  [[nodiscard]] uint64_t gas_remaining() const { return gas_remaining_; }
  [[nodiscard]] Trap trap() const { return trap_; }
  [[nodiscard]] bool halted() const { return halted_; }
  [[nodiscard]] uint32_t halt_code() const { return halt_code_; }
  [[nodiscard]] std::span<const uint8_t> memory() const { return memory_; }

  // x0 is hardwired to zero. `i` is an unchecked index: callers pass a decoded
  // 5-bit register field (< kRegisterCount), so no bounds check on the hot
  // path.
  void write_reg(uint8_t i, uint32_t v)
  {
    if (i != 0) {
      regs_[i] = v;
    }
  }

  // Public so free-function handlers (below) can manipulate state. This is an
  // experimental, single-translation-unit interpreter; the handlers are
  // effectively part of the machine.
  std::shared_ptr<const ThreadedProgram> program_;
  std::array<uint32_t, kRegisterCount> regs_ = {};
  std::vector<uint8_t> memory_;
  uint32_t initial_pc_ = 0;
  uint32_t pc_ = 0;
  uint64_t gas_remaining_ = 0;
  Trap trap_ = Trap::kNone;
  Trap construction_trap_ = Trap::kNone;
  bool halted_ = false;
  uint32_t halt_code_ = 0;
};

template<class Obs> struct Ctx;
template<class Obs> using Handler = StepResult (*)(Ctx<Obs> &);

template<class Obs> struct Ctx {
  ThreadedMachine &m;
  Host &host;
  Obs &obs;
  const Instr *code = nullptr;
  const Handler<Obs> *handlers =
      nullptr; // size n+1; [n] is the bad-pc sentinel
  size_t idx = 0;
};

template<class Obs>
inline ArchState
capture(const Ctx<Obs> &c)
{
  ArchState s;
  s.pc = c.m.pc_;
  s.regs = c.m.regs_;
  s.memory.assign(c.m.memory_.begin(), c.m.memory_.end());
  s.gas_remaining = c.m.gas_remaining_;
  s.trap = c.m.trap_;
  s.halted = c.m.halted_;
  s.halt_code = c.m.halt_code_;
  return s;
}

// Tail-dispatch to the next handler, after a per-step observation. A macro (not
// an inlined helper) so HOGD_MUSTTAIL applies to the real tail call.
#define HOGD_THREAD_NEXT(c)                                                    \
  do {                                                                         \
    if constexpr (Obs::kEnabled)                                               \
      (c).obs.on_retire(capture(c), StepResult::kContinue);                    \
    HOGD_MUSTTAIL return (c).handlers[(c).idx]((c));                           \
  } while (0)

// Charge gas; on failure set kOutOfGas, observe a terminal step, return kTrap.
// `amount` is evaluated twice; pass a side-effect-free expression (call sites
// pass the plain `in.gas` field).
#define HOGD_CHARGE_OR_TRAP(c, amount)                                         \
  do {                                                                         \
    if ((c).m.gas_remaining_ < (amount)) {                                     \
      (c).m.trap_ = Trap::kOutOfGas;                                           \
      (c).m.halted_ = true;                                                    \
      if constexpr (Obs::kEnabled)                                             \
        (c).obs.on_retire(capture(c), StepResult::kTrap);                      \
      return StepResult::kTrap;                                                \
    }                                                                          \
    (c).m.gas_remaining_ -= (amount);                                          \
  } while (0)

template<class Obs>
inline StepResult
trap_step(Ctx<Obs> &c, Trap t)
{
  c.m.trap_ = t;
  c.m.halted_ = true;
  if constexpr (Obs::kEnabled) {
    c.obs.on_retire(capture(c), StepResult::kTrap);
  }
  return StepResult::kTrap;
}

// Pure ALU operations, shared by R-type and I-type handlers.
inline uint32_t
add_op(uint32_t a, uint32_t b)
{
  return a + b;
}
inline uint32_t
sub_op(uint32_t a, uint32_t b)
{
  return a - b;
}
inline uint32_t
and_op(uint32_t a, uint32_t b)
{
  return a & b;
}
inline uint32_t
or_op(uint32_t a, uint32_t b)
{
  return a | b;
}
inline uint32_t
xor_op(uint32_t a, uint32_t b)
{
  return a ^ b;
}
inline uint32_t
sll_op(uint32_t a, uint32_t b)
{
  return a << (b & 0x1fU);
}
inline uint32_t
srl_op(uint32_t a, uint32_t b)
{
  return a >> (b & 0x1fU);
}
inline uint32_t
sra_op(uint32_t a, uint32_t b)
{
  return arithmetic_shift_right(a, b & 0x1fU);
}
inline uint32_t
slt_op(uint32_t a, uint32_t b)
{
  return i32_from_bits(a) < i32_from_bits(b) ? 1U : 0U;
}
inline uint32_t
sltu_op(uint32_t a, uint32_t b)
{
  return a < b ? 1U : 0U;
}
inline uint32_t
mul_op(uint32_t a, uint32_t b)
{
  return static_cast<uint32_t>(uint64_t{a} * uint64_t{b});
}
inline uint32_t
divu_op(uint32_t a, uint32_t b)
{
  return b == 0 ? std::numeric_limits<uint32_t>::max() : a / b;
}
inline uint32_t
remu_op(uint32_t a, uint32_t b)
{
  return b == 0 ? a : a % b;
}

template<class Obs, uint32_t (*Fn)(uint32_t, uint32_t)>
StepResult
op_rr(Ctx<Obs> &c)
{
  const Instr &in = c.code[c.idx];
  HOGD_CHARGE_OR_TRAP(c, in.gas);
  c.m.write_reg(in.rd, Fn(c.m.regs_[in.rs1], c.m.regs_[in.rs2]));
  c.idx += 1;
  c.m.pc_ = static_cast<uint32_t>(c.idx) * kInstructionBytes;
  HOGD_THREAD_NEXT(c);
}

template<class Obs, uint32_t (*Fn)(uint32_t, uint32_t)>
StepResult
op_ri(Ctx<Obs> &c)
{
  const Instr &in = c.code[c.idx];
  HOGD_CHARGE_OR_TRAP(c, in.gas);
  c.m.write_reg(in.rd, Fn(c.m.regs_[in.rs1], bits_from_i32(in.imm)));
  c.idx += 1;
  c.m.pc_ = static_cast<uint32_t>(c.idx) * kInstructionBytes;
  HOGD_THREAD_NEXT(c);
}

template<class Obs>
StepResult
op_lui(Ctx<Obs> &c)
{
  const Instr &in = c.code[c.idx];
  HOGD_CHARGE_OR_TRAP(c, in.gas);
  c.m.write_reg(in.rd, bits_from_i32(in.imm));
  c.idx += 1;
  c.m.pc_ = static_cast<uint32_t>(c.idx) * kInstructionBytes;
  HOGD_THREAD_NEXT(c);
}

template<class Obs>
StepResult
op_auipc(Ctx<Obs> &c)
{
  const Instr &in = c.code[c.idx];
  HOGD_CHARGE_OR_TRAP(c, in.gas);
  c.m.write_reg(in.rd, c.m.pc_ + bits_from_i32(in.imm));
  c.idx += 1;
  c.m.pc_ = static_cast<uint32_t>(c.idx) * kInstructionBytes;
  HOGD_THREAD_NEXT(c);
}

// Validate a byte target against the program image. Mirrors
// Machine::validate_control_target via ThreadedProgram::contains_pc.
template<class Obs>
inline bool
valid_target(Ctx<Obs> &c, uint32_t target)
{
  // contains_pc already checks 4-byte alignment and in-range, so delegate to it
  // wholesale -- same as Machine::validate_control_target.
  return c.m.program_->contains_pc(target);
}

template<class Obs>
StepResult
op_branch(Ctx<Obs> &c)
{
  const Instr &in = c.code[c.idx];
  HOGD_CHARGE_OR_TRAP(c, in.gas);
  if (!branch_taken(in.op, c.m.regs_[in.rs1], c.m.regs_[in.rs2])) {
    c.idx += 1;
    c.m.pc_ = static_cast<uint32_t>(c.idx) * kInstructionBytes;
    HOGD_THREAD_NEXT(c);
  }
  // Taken: pc_ still holds this branch's address (the not-taken path returned
  // before advancing), so target = this-pc + imm.
  const uint32_t target = c.m.pc_ + bits_from_i32(in.imm);
  if (!valid_target(c, target)) {
    return trap_step(c, Trap::kBadPc);
  }
  c.m.pc_ = target;
  c.idx = target / kInstructionBytes;
  HOGD_THREAD_NEXT(c);
}

template<class Obs>
StepResult
op_jal(Ctx<Obs> &c)
{
  const Instr &in = c.code[c.idx];
  HOGD_CHARGE_OR_TRAP(c, in.gas);
  const uint32_t link = c.m.pc_ + kInstructionBytes;
  const uint32_t target = c.m.pc_ + bits_from_i32(in.imm);
  if (!valid_target(c, target)) {
    return trap_step(c, Trap::kBadPc);
  }
  c.m.write_reg(in.rd, link);
  c.m.pc_ = target;
  c.idx = target / kInstructionBytes;
  HOGD_THREAD_NEXT(c);
}

template<class Obs>
StepResult
op_jalr(Ctx<Obs> &c)
{
  const Instr &in = c.code[c.idx];
  HOGD_CHARGE_OR_TRAP(c, in.gas);
  const uint32_t link = c.m.pc_ + kInstructionBytes;
  const uint32_t target =
      (c.m.regs_[in.rs1] + bits_from_i32(in.imm)) & ~uint32_t{1};
  if (!valid_target(c, target)) {
    return trap_step(c, Trap::kBadPc);
  }
  c.m.write_reg(in.rd, link);
  c.m.pc_ = target;
  c.idx = target / kInstructionBytes;
  HOGD_THREAD_NEXT(c);
}

// Sentinel: reached only by sequential fall-off past the last instruction.
// Traps kBadPc with NO gas charged (matches "invalid fetch traps without gas").
template<class Obs>
StepResult
op_bad_pc(Ctx<Obs> &c)
{
  return trap_step(c, Trap::kBadPc);
}

// Default for ops not yet wired (control flow, memory, ecall arrive in later
// tasks). Charges no gas; replaced as those tasks land.
template<class Obs>
StepResult
op_invalid(Ctx<Obs> &c)
{
  return trap_step(c, Trap::kInvalidInstruction);
}

template<class Obs>
Handler<Obs>
handler_for(Op op)
{
  switch (op) {
  case Op::kJal:
    return &op_jal<Obs>;
  case Op::kJalr:
    return &op_jalr<Obs>;
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
    return &op_branch<Obs>;
  case Op::kLui:
    return &op_lui<Obs>;
  case Op::kAuipc:
    return &op_auipc<Obs>;
  case Op::kAddi:
    return &op_ri<Obs, add_op>;
  case Op::kSlti:
    return &op_ri<Obs, slt_op>;
  case Op::kSltiu:
    return &op_ri<Obs, sltu_op>;
  case Op::kXori:
    return &op_ri<Obs, xor_op>;
  case Op::kOri:
    return &op_ri<Obs, or_op>;
  case Op::kAndi:
    return &op_ri<Obs, and_op>;
  case Op::kSlli:
    return &op_ri<Obs, sll_op>;
  case Op::kSrli:
    return &op_ri<Obs, srl_op>;
  case Op::kSrai:
    return &op_ri<Obs, sra_op>;
  case Op::kAdd:
    return &op_rr<Obs, add_op>;
  case Op::kSub:
    return &op_rr<Obs, sub_op>;
  case Op::kSll:
    return &op_rr<Obs, sll_op>;
  case Op::kSlt:
    return &op_rr<Obs, slt_op>;
  case Op::kSltu:
    return &op_rr<Obs, sltu_op>;
  case Op::kXor:
    return &op_rr<Obs, xor_op>;
  case Op::kSrl:
    return &op_rr<Obs, srl_op>;
  case Op::kSra:
    return &op_rr<Obs, sra_op>;
  case Op::kOr:
    return &op_rr<Obs, or_op>;
  case Op::kAnd:
    return &op_rr<Obs, and_op>;
  case Op::kMul:
    return &op_rr<Obs, mul_op>;
  case Op::kMulh:
    return &op_rr<Obs, mulh>;
  case Op::kMulhsu:
    return &op_rr<Obs, mulhsu>;
  case Op::kMulhu:
    return &op_rr<Obs, mulhu>;
  case Op::kDiv:
    return &op_rr<Obs, div_signed>;
  case Op::kDivu:
    return &op_rr<Obs, divu_op>;
  case Op::kRem:
    return &op_rr<Obs, rem_signed>;
  case Op::kRemu:
    return &op_rr<Obs, remu_op>;
  default:
    return &op_invalid<Obs>; // control flow / memory / ecall wired in Tasks 5-7
  }
}

template<class Obs>
StepResult
ThreadedMachine::run(Host &host, Obs &obs)
{
  if (halted_) {
    return trap_ == Trap::kNone ? StepResult::kHalted : StepResult::kTrap;
  }
  const std::span<const Instr> code = program_->code();
  const size_t n = code.size();
  std::vector<Handler<Obs>> handlers(n + 1);
  for (size_t i = 0; i < n; ++i) {
    handlers[i] = handler_for<Obs>(code[i].op);
  }
  handlers[n] = &op_bad_pc<Obs>;

  Ctx<Obs> c{.m = *this,
             .host = host,
             .obs = obs,
             .code = code.data(),
             .handlers = handlers.data(),
             .idx = pc_ / kInstructionBytes};
  if constexpr (Obs::kEnabled) {
    obs.on_initial(capture(c));
  }
  // store-then-return (NOT a tail call): keeps this frame (owning `handlers`
  // and `c`) alive for the whole musttail chain.
  const StepResult result = c.handlers[c.idx](c);
  return result;
}

} // namespace hogd::rv32
