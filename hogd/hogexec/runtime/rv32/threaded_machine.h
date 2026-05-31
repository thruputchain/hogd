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

// Direct-threaded, musttail RV32IM interpreter. Handlers take the current
// instruction pointer `ip` and tail-call (ip + 1) in the common case, so there
// is no instruction-index arithmetic and no per-instruction pc update; pc is
// derived as (ip - base) * 4 only by the control-flow handlers and when
// materializing observable state.
//
// Gas is metered at *start of run* (block granularity, like the wasm engines):
// a "run" is a maximal sequence of non-barrier instructions (ALU/lui/auipc/
// fence -- none of which trap or branch). enter_run() charges the whole run's
// gas in one check; the non-barrier handlers inside do no gas work. Barriers
// (loads/stores/branches/jal/jalr/ecall/ebreak) end a run and self-charge their
// own gas. On out-of-gas, the run is aborted at its head -- so the exact
// instruction and intermediate gas_remaining differ from a per-instruction
// machine, but total gas to any barrier (and thus to any ecall and to halt) is
// identical, which is all that persists.
//
// Tracing is a build-time choice: handlers are templated on a `Trace` bool, and
// the executable Slot array is built (once, cached across reset()) with either
// the untraced (production) or traced (records an ExecTrace) handler set.
namespace hogd::rv32 {

struct Ctx;
struct Slot;
using Handler = StepResult (*)(Ctx, const Slot *);

// Executable instruction: handler pointer + decoded operands + gas. `gas` is
// this instruction's own cost (charged by barriers); `gas_to_run` is the total
// cost of the non-barrier run starting here (charged once by enter_run), 0 for
// a barrier. 24 bytes.
struct Slot {
  Handler handler = nullptr;
  uint32_t gas = static_cast<uint32_t>(kBaseInstructionGas);
  uint32_t gas_to_run = 0;
  int32_t imm = 0;
  Op op = Op::kAdd; // only op_branch reads this (to pick the comparison)
  uint8_t rd = 0;
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
};

class ThreadedMachine {
public:
  ThreadedMachine(std::shared_ptr<const ThreadedProgram> program,
                  uint32_t memory_bytes, uint64_t gas, uint32_t initial_pc = 0)
    : program_(std::move(program)), initial_pc_(initial_pc)
  {
    if (!program_) {
      construction_trap_ = Trap::kInvalidInstruction;
      trap_ = Trap::kInvalidInstruction;
      halted_ = true;
      return;
    }
    if (!program_->contains_pc(initial_pc_)) {
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
    // exec_ intentionally preserved across reset(): the program is immutable,
    // so the baked handler array stays valid and is reused.
  }

  // Production fast path: untraced handlers, no per-step observation.
  StepResult run(Host &host);
  // Differential/tracing path: records each retired step into `obs`.
  StepResult run(Host &host, TraceObserver &obs);

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

  // x0 is hardwired to zero. Branchless sink: write unconditionally, then force
  // regs_[0] back to 0 -- avoids a per-instruction branch on rd. `i` is an
  // unchecked index: callers pass a decoded 5-bit register field
  // (< kRegisterCount), so no bounds check on the hot path.
  void write_reg(uint8_t i, uint32_t v)
  {
    regs_[i] = v;
    regs_[0] = 0;
  }

  // Build (once, then cached) the executable Slot array from the decoded
  // program, baking the traced or untraced handler set per `trace`. Defined
  // out-of-line below, after the handler functions it references.
  void build_exec(bool trace);

  // Public so the free-function handlers (below) can manipulate state. This is
  // an experimental, single-translation-unit interpreter; the handlers are
  // effectively part of the machine.
  //
  // regs_ is deliberately the first member (offset 0): the machine pointer is
  // then also the register-file base, so hot-path regs_[i] is a single indexed
  // load with no base adjustment, while every other (cold) field is still
  // reached through the same pointer at its own offset.
  std::array<uint32_t, kRegisterCount> regs_ = {};
  std::shared_ptr<const ThreadedProgram> program_;
  std::vector<uint8_t> memory_;
  std::vector<Slot> exec_; // size n+1; [n] is the bad-pc sentinel
  Host *host_ = nullptr;   // set for the duration of run(); cold (ecall only)
  TraceObserver *obs_ = nullptr; // set on the traced run(); cold
  uint32_t initial_pc_ = 0;
  uint32_t pc_ = 0;
  uint64_t gas_remaining_ = 0;
  Trap trap_ = Trap::kNone;
  Trap construction_trap_ = Trap::kNone;
  bool halted_ = false;
  bool exec_built_ = false;
  bool exec_traced_ = false;
  uint32_t halt_code_ = 0;
};

// Passed BY VALUE through the musttail chain: two pointers fit in two registers
// (AArch64), so the machine pointer and run base stay in registers across the
// whole run instead of being reloaded from memory on every handler. Cold state
// (host/obs) lives on the machine, reached via m.
struct Ctx {
  ThreadedMachine *m = nullptr;
  const Slot *base = nullptr; // exec_.data(); pc = (ip - base) * 4
};

// pc of the instruction at `ip`. Cheap (a multiply); called only by the
// control-flow handlers and when materializing observable state.
inline uint32_t
pc_of(const Ctx c, const Slot *ip)
{
  return static_cast<uint32_t>(ip - c.base) * kInstructionBytes;
}

inline ArchState
capture(const Ctx c)
{
  ArchState s;
  s.pc = c.m->pc_;
  s.regs = c.m->regs_;
  s.memory.assign(c.m->memory_.begin(), c.m->memory_.end());
  s.gas_remaining = c.m->gas_remaining_;
  s.trap = c.m->trap_;
  s.halted = c.m->halted_;
  s.halt_code = c.m->halt_code_;
  return s;
}

// A barrier ends a gas run: it transfers control (branches/jumps/ecall/ebreak),
// so the run can't be pre-charged past it (the continuation isn't known). Its
// gas is charged individually. Loads/stores are NOT barriers: they're
// straight-line, so they fold into runs; a mid-run fault is terminal and has
// already paid the run's gas (block-granular metering, like the wasm engines).
inline bool
is_barrier(Op op)
{
  switch (op) {
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
  case Op::kJal:
  case Op::kJalr:
  case Op::kEcall:
  case Op::kEbreak:
    return true;
  default:
    return false;
  }
}

// Tail-dispatch to the next instruction in the *same* run (no gas work; the run
// was pre-charged by enter_run). On the traced path, materialize pc_ and record
// the retired step first. `Trace` is the enclosing handler's template
// parameter.
#define HOGD_THREAD_NEXT(c, next)                                              \
  if constexpr (Trace) {                                                       \
    (c).m->pc_ = pc_of((c), (next));                                           \
    (c).m->obs_->on_retire(capture(c), StepResult::kContinue);                 \
  }                                                                            \
  HOGD_MUSTTAIL return (next)->handler((c), (next))

// Tail-dispatch that *starts a new run* at `next` (after a barrier): record the
// retired step (traced), then enter_run charges the new run's gas.
#define HOGD_ENTER_RUN(c, next)                                                \
  if constexpr (Trace) {                                                       \
    (c).m->pc_ = pc_of((c), (next));                                           \
    (c).m->obs_->on_retire(capture(c), StepResult::kContinue);                 \
  }                                                                            \
  HOGD_MUSTTAIL return enter_run<Trace>((c), (next))

// Charge a single instruction's gas (barriers); on failure set kOutOfGas at
// `ip`'s pc, observe a terminal step, return kTrap. `amount` is evaluated
// twice; pass a side-effect-free expression.
#define HOGD_CHARGE_OR_TRAP(c, ip, amount)                                     \
  if ((c).m->gas_remaining_ < (amount)) {                                      \
    (c).m->pc_ = pc_of((c), (ip));                                             \
    (c).m->trap_ = Trap::kOutOfGas;                                            \
    (c).m->halted_ = true;                                                     \
    if constexpr (Trace)                                                       \
      (c).m->obs_->on_retire(capture(c), StepResult::kTrap);                   \
    return StepResult::kTrap;                                                  \
  }                                                                            \
  (c).m->gas_remaining_ -= (amount)

// Set a terminal trap at `pc`, observe it, return kTrap.
template<bool Trace>
inline StepResult
trap_at(Ctx c, uint32_t pc, Trap t)
{
  c.m->pc_ = pc;
  c.m->trap_ = t;
  c.m->halted_ = true;
  if constexpr (Trace) {
    c.m->obs_->on_retire(capture(c), StepResult::kTrap);
  }
  return StepResult::kTrap;
}

// Charge the gas for the whole non-barrier run starting at `ip`, then dispatch
// to it. Called at every run head: program start, every jump/branch target, and
// the instruction after each barrier. Block-granular OOG: if the run does not
// fit, abort at its head (total gas to the next barrier is what persists). For
// a barrier, gas_to_run is 0 and the barrier self-charges.
template<bool Trace>
StepResult
enter_run(Ctx c, const Slot *ip)
{
  const uint32_t run_gas = ip->gas_to_run;
  if (c.m->gas_remaining_ < run_gas) {
    c.m->pc_ = pc_of(c, ip);
    c.m->trap_ = Trap::kOutOfGas;
    c.m->halted_ = true;
    if constexpr (Trace) {
      c.m->obs_->on_retire(capture(c), StepResult::kTrap);
    }
    return StepResult::kTrap;
  }
  c.m->gas_remaining_ -= run_gas;
  HOGD_MUSTTAIL return ip->handler(c, ip);
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

// --- Non-barrier handlers: no gas work (the run was pre-charged) ------------

template<bool Trace, uint32_t (*Fn)(uint32_t, uint32_t)>
StepResult
op_rr(Ctx c, const Slot *ip)
{
  c.m->write_reg(ip->rd, Fn(c.m->regs_[ip->rs1], c.m->regs_[ip->rs2]));
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace, uint32_t (*Fn)(uint32_t, uint32_t)>
StepResult
op_ri(Ctx c, const Slot *ip)
{
  c.m->write_reg(ip->rd, Fn(c.m->regs_[ip->rs1], bits_from_i32(ip->imm)));
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace>
StepResult
op_lui(Ctx c, const Slot *ip)
{
  c.m->write_reg(ip->rd, bits_from_i32(ip->imm));
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace>
StepResult
op_auipc(Ctx c, const Slot *ip)
{
  c.m->write_reg(ip->rd, pc_of(c, ip) + bits_from_i32(ip->imm));
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace>
StepResult
op_fence(Ctx c, const Slot *ip)
{
  HOGD_THREAD_NEXT(c, ip + 1);
}

// --- Barrier handlers: self-charge gas, end the run -------------------------

// Validate a byte target against the program image. Mirrors
// Machine::validate_control_target via ThreadedProgram::contains_pc, which
// already checks 4-byte alignment and in-range.
inline bool
valid_target(Ctx c, uint32_t target)
{
  return c.m->program_->contains_pc(target);
}

template<bool Trace>
StepResult
op_branch(Ctx c, const Slot *ip)
{
  HOGD_CHARGE_OR_TRAP(c, ip, ip->gas);
  if (!branch_taken(ip->op, c.m->regs_[ip->rs1], c.m->regs_[ip->rs2])) {
    HOGD_ENTER_RUN(c, ip + 1);
  }
  const uint32_t target = pc_of(c, ip) + bits_from_i32(ip->imm);
  if (!valid_target(c, target)) {
    return trap_at<Trace>(c, pc_of(c, ip), Trap::kBadPc);
  }
  const Slot *next = c.base + (target / kInstructionBytes);
  HOGD_ENTER_RUN(c, next);
}

template<bool Trace>
StepResult
op_jal(Ctx c, const Slot *ip)
{
  HOGD_CHARGE_OR_TRAP(c, ip, ip->gas);
  const uint32_t pc = pc_of(c, ip);
  const uint32_t target = pc + bits_from_i32(ip->imm);
  if (!valid_target(c, target)) {
    return trap_at<Trace>(c, pc, Trap::kBadPc);
  }
  c.m->write_reg(ip->rd, pc + kInstructionBytes);
  const Slot *next = c.base + (target / kInstructionBytes);
  HOGD_ENTER_RUN(c, next);
}

template<bool Trace>
StepResult
op_jalr(Ctx c, const Slot *ip)
{
  HOGD_CHARGE_OR_TRAP(c, ip, ip->gas);
  const uint32_t pc = pc_of(c, ip);
  const uint32_t target =
      (c.m->regs_[ip->rs1] + bits_from_i32(ip->imm)) & ~uint32_t{1};
  if (!valid_target(c, target)) {
    return trap_at<Trace>(c, pc, Trap::kBadPc);
  }
  c.m->write_reg(ip->rd, pc + kInstructionBytes);
  const Slot *next = c.base + (target / kInstructionBytes);
  HOGD_ENTER_RUN(c, next);
}

// Width-parameterized little-endian memory helpers. Not shared with
// spec::load_le (the spec namespace is independent test infrastructure) or
// encoding.h's load_u32_le (fixed 4-byte width); a local copy keeps the
// interpreter self-contained and width-generic.
inline uint32_t
le_load(std::span<const uint8_t> mem, uint32_t addr, uint32_t w)
{
  uint32_t v = 0;
  for (uint32_t i = 0; i < w; ++i) {
    v |= uint32_t{mem[addr + i]} << (i * 8);
  }
  return v;
}

inline void
le_store(std::span<uint8_t> mem, uint32_t addr, uint32_t w, uint32_t v)
{
  for (uint32_t i = 0; i < w; ++i) {
    mem[addr + i] = static_cast<uint8_t>(v >> (i * 8));
  }
}

// Loads/stores are non-barriers: no self-charge (their gas is in the run total
// pre-charged by enter_run), and they continue in-run via THREAD_NEXT. They can
// still fault (misaligned/oob), which is terminal.
template<bool Trace, uint32_t Width, bool SignExtend>
StepResult
op_load(Ctx c, const Slot *ip)
{
  const uint32_t addr = c.m->regs_[ip->rs1] + bits_from_i32(ip->imm);
  if ((addr % Width) != 0) {
    return trap_at<Trace>(c, pc_of(c, ip), Trap::kMisalignedAccess);
  }
  if (!memory_range_in_bounds(c.m->memory_.size(), addr, Width)) {
    return trap_at<Trace>(c, pc_of(c, ip), Trap::kBadMemory);
  }
  uint32_t v = le_load(c.m->memory_, addr, Width);
  if constexpr (SignExtend) {
    v = sign_extend(v, Width * 8);
  }
  c.m->write_reg(ip->rd, v);
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace, uint32_t Width>
StepResult
op_store(Ctx c, const Slot *ip)
{
  const uint32_t addr = c.m->regs_[ip->rs1] + bits_from_i32(ip->imm);
  if ((addr % Width) != 0) {
    return trap_at<Trace>(c, pc_of(c, ip), Trap::kMisalignedAccess);
  }
  if (!memory_range_in_bounds(c.m->memory_.size(), addr, Width)) {
    return trap_at<Trace>(c, pc_of(c, ip), Trap::kBadMemory);
  }
  le_store(c.m->memory_, addr, Width, c.m->regs_[ip->rs2]);
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace>
StepResult
op_ebreak(Ctx c, const Slot *ip)
{
  HOGD_CHARGE_OR_TRAP(c, ip, ip->gas); // ebreak charges gas, then traps
  return trap_at<Trace>(c, pc_of(c, ip), Trap::kBreakpoint);
}

// op_ecall resolves the host call inline (the threaded machine runs to
// completion without a kEcall yield). The HostResult is evaluated in an inner
// scope so that the memory snapshot and host result -- both non-trivially
// destructible -- are destroyed before the musttail in the kContinue path,
// which the compiler requires.
template<bool Trace>
StepResult
op_ecall(Ctx c, const Slot *ip)
{
  // ECALL instruction gas (canonical charges it in step(), before execute()).
  // On OOG here, pc stays at the ecall.
  HOGD_CHARGE_OR_TRAP(c, ip, ip->gas);

  // pc advances past the ecall before the host call (canonical ordering).
  const Slot *next = ip + 1;
  const uint32_t pc_after = pc_of(c, next);

  uint32_t continue_value = 0;
  {
    // Snapshot memory for host-OOG rollback; destroyed before the musttail.
    const std::vector<uint8_t> snapshot = c.m->memory_;
    const EcallArgs args{.a0 = c.m->regs_[10],
                         .a1 = c.m->regs_[11],
                         .a2 = c.m->regs_[12],
                         .a3 = c.m->regs_[13],
                         .a4 = c.m->regs_[14],
                         .a5 = c.m->regs_[15],
                         .a6 = c.m->regs_[16],
                         .a7 = c.m->regs_[17]};
    // view reflects the already-advanced pc, matching canonical.
    MachineView view(pc_after, c.m->gas_remaining_,
                     std::span<const uint32_t, kRegisterCount>(c.m->regs_),
                     c.m->memory_);
    const HostResult hr = c.m->host_->ecall(view, args);

    // Debit host gas before committing; over-budget host cannot mutate state.
    if (c.m->gas_remaining_ < hr.gas_consumed) {
      c.m->memory_ = snapshot;
      return trap_at<Trace>(c, pc_after, Trap::kOutOfGas);
    }
    c.m->gas_remaining_ -= hr.gas_consumed;

    switch (hr.status) {
    case HostStatus::kContinue:
      continue_value = hr.continue_value();
      break;
    case HostStatus::kHalt:
      c.m->pc_ = pc_after;
      c.m->halt_code_ = hr.halt_code();
      c.m->halted_ = true;
      if constexpr (Trace) {
        c.m->obs_->on_retire(capture(c), StepResult::kHalted);
      }
      return StepResult::kHalted;
    case HostStatus::kOutOfGas:
      c.m->memory_ = snapshot;
      return trap_at<Trace>(c, pc_after, Trap::kOutOfGas);
    case HostStatus::kTrap:
    default:
      // Canonical set_host_trap also records a HostTrapPayload; ThreadedMachine
      // has no payload field (ArchState carries only the Trap, which is what
      // the differential test compares), so just set the trap class.
      return trap_at<Trace>(c, pc_after, Trap::kHostTrap);
    }
  } // snapshot and hr destroyed here

  // kContinue: write a0 and start the next run.
  c.m->write_reg(10, continue_value);
  HOGD_ENTER_RUN(c, next);
}

// Sentinel: reached only by sequential fall-off past the last instruction. Its
// pc is (ip - base) * 4 == the out-of-range pc. Traps kBadPc with NO gas
// charged (matches "invalid fetch traps without gas").
template<bool Trace>
StepResult
op_bad_pc(Ctx c, const Slot *ip)
{
  return trap_at<Trace>(c, pc_of(c, ip), Trap::kBadPc);
}

// Defensive default for handler_for; unreachable by construction, since
// ThreadedProgram::create rejects every unsupported encoding, so no Instr ever
// carries an unhandled Op. Charges no gas.
template<bool Trace>
StepResult
op_invalid(Ctx c, const Slot *ip)
{
  return trap_at<Trace>(c, pc_of(c, ip), Trap::kInvalidInstruction);
}

template<bool Trace>
Handler
handler_for(Op op)
{
  switch (op) {
  case Op::kJal:
    return &op_jal<Trace>;
  case Op::kJalr:
    return &op_jalr<Trace>;
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
    return &op_branch<Trace>;
  case Op::kLui:
    return &op_lui<Trace>;
  case Op::kAuipc:
    return &op_auipc<Trace>;
  case Op::kAddi:
    return &op_ri<Trace, add_op>;
  case Op::kSlti:
    return &op_ri<Trace, slt_op>;
  case Op::kSltiu:
    return &op_ri<Trace, sltu_op>;
  case Op::kXori:
    return &op_ri<Trace, xor_op>;
  case Op::kOri:
    return &op_ri<Trace, or_op>;
  case Op::kAndi:
    return &op_ri<Trace, and_op>;
  case Op::kSlli:
    return &op_ri<Trace, sll_op>;
  case Op::kSrli:
    return &op_ri<Trace, srl_op>;
  case Op::kSrai:
    return &op_ri<Trace, sra_op>;
  case Op::kAdd:
    return &op_rr<Trace, add_op>;
  case Op::kSub:
    return &op_rr<Trace, sub_op>;
  case Op::kSll:
    return &op_rr<Trace, sll_op>;
  case Op::kSlt:
    return &op_rr<Trace, slt_op>;
  case Op::kSltu:
    return &op_rr<Trace, sltu_op>;
  case Op::kXor:
    return &op_rr<Trace, xor_op>;
  case Op::kSrl:
    return &op_rr<Trace, srl_op>;
  case Op::kSra:
    return &op_rr<Trace, sra_op>;
  case Op::kOr:
    return &op_rr<Trace, or_op>;
  case Op::kAnd:
    return &op_rr<Trace, and_op>;
  case Op::kMul:
    return &op_rr<Trace, mul_op>;
  case Op::kMulh:
    return &op_rr<Trace, mulh>;
  case Op::kMulhsu:
    return &op_rr<Trace, mulhsu>;
  case Op::kMulhu:
    return &op_rr<Trace, mulhu>;
  case Op::kDiv:
    return &op_rr<Trace, div_signed>;
  case Op::kDivu:
    return &op_rr<Trace, divu_op>;
  case Op::kRem:
    return &op_rr<Trace, rem_signed>;
  case Op::kRemu:
    return &op_rr<Trace, remu_op>;
  case Op::kLb:
    return &op_load<Trace, 1, true>;
  case Op::kLh:
    return &op_load<Trace, 2, true>;
  case Op::kLw:
    return &op_load<Trace, 4, false>;
  case Op::kLbu:
    return &op_load<Trace, 1, false>;
  case Op::kLhu:
    return &op_load<Trace, 2, false>;
  case Op::kSb:
    return &op_store<Trace, 1>;
  case Op::kSh:
    return &op_store<Trace, 2>;
  case Op::kSw:
    return &op_store<Trace, 4>;
  case Op::kFence:
    return &op_fence<Trace>;
  case Op::kEbreak:
    return &op_ebreak<Trace>;
  case Op::kEcall:
    return &op_ecall<Trace>;
  default:
    return &op_invalid<Trace>;
  }
}

inline void
ThreadedMachine::build_exec(bool trace)
{
  const std::span<const Instr> code = program_->code();
  const size_t n = code.size();
  exec_.resize(n + 1);
  for (size_t i = 0; i < n; ++i) {
    const Instr &d = code[i];
    exec_[i] = Slot{.handler = trace ? handler_for<true>(d.op)
                                     : handler_for<false>(d.op),
                    .gas = static_cast<uint32_t>(d.gas),
                    .gas_to_run = 0, // filled by the backward pass below
                    .imm = d.imm,
                    .op = d.op,
                    .rd = d.rd,
                    .rs1 = d.rs1,
                    .rs2 = d.rs2};
  }
  // Sentinel for sequential fall-off past the last instruction.
  exec_[n] = Slot{.handler = trace ? &op_bad_pc<true> : &op_bad_pc<false>};
  // gas_to_run[i] = gas of the maximal non-barrier run starting at i (0 for a
  // barrier); backward so each entry sees its successor's already-computed sum.
  for (size_t i = n; i-- > 0;) {
    if (is_barrier(exec_[i].op)) {
      exec_[i].gas_to_run = 0;
    }
    else {
      const bool next_in_run = (i + 1 < n) && !is_barrier(exec_[i + 1].op);
      exec_[i].gas_to_run =
          exec_[i].gas + (next_in_run ? exec_[i + 1].gas_to_run : 0);
    }
  }
  exec_built_ = true;
  exec_traced_ = trace;
}

inline StepResult
ThreadedMachine::run(Host &host)
{
  if (halted_) {
    return trap_ == Trap::kNone ? StepResult::kHalted : StepResult::kTrap;
  }
  if (!exec_built_ || exec_traced_) {
    build_exec(/*trace=*/false);
  }
  host_ = &host;
  obs_ = nullptr;
  Ctx c{.m = this, .base = exec_.data()};
  const Slot *ip = c.base + (pc_ / kInstructionBytes);
  // store-then-return (NOT a tail call): keeps this frame (owning `c`) alive
  // for the whole musttail chain.
  const StepResult result = enter_run<false>(c, ip);
  return result;
}

inline StepResult
ThreadedMachine::run(Host &host, TraceObserver &obs)
{
  if (halted_) {
    return trap_ == Trap::kNone ? StepResult::kHalted : StepResult::kTrap;
  }
  if (!exec_built_ || !exec_traced_) {
    build_exec(/*trace=*/true);
  }
  host_ = &host;
  obs_ = &obs;
  Ctx c{.m = this, .base = exec_.data()};
  const Slot *ip = c.base + (pc_ / kInstructionBytes);
  obs.on_initial(capture(c));
  const StepResult result = enter_run<true>(c, ip);
  return result;
}

} // namespace hogd::rv32
