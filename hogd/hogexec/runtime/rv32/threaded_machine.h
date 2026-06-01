#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
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
using Handler = StepResult (*)(Ctx, const Slot *, uint8_t *, uint32_t, uint64_t);

// Executable instruction, packed to 16 bytes (alignas(16) -- a power of two, so
// `ip - base` (pc_of) and `ip + imm` (branch/jump targets) still lower to a
// shift, and the exec_ array is half the size of the old 32-byte slot). Layout:
//   handler (8) | gas_to_run (2) | rd (1) | rs1 (1) | union{imm; {imm16,rs2}}(4)
// rd/rs1/rs2 are register indices; rd==0 maps to a trash slot at
// regs_[kRegisterCount] so the write is unconditional (no x0 special-case).
// The union exploits that no instruction needs rs2 together with a >16-bit imm:
// only lui/auipc/jal carry a wide imm and none of them use rs2, while the
// rs2-users (R-type/store/branch) need at most an int16 imm (store offset /
// branch slot-delta). So rs2 shares the imm word with a narrow imm16.
// `gas_to_run` is the run's total gas (body through terminating barrier),
// charged once by enter_run; uint16 -- build_exec rejects a program whose run
// gas would overflow (TODO: split the run / charge in chunks instead).
struct alignas(16) Slot {
  Handler handler = nullptr; // offset 0
  uint16_t gas_to_run = 0;   // offset 8
  uint8_t rd = 0;            // offset 10: dest reg index (0 -> kTrashReg)
  uint8_t rs1 = 0;           // offset 11
  union {                    // offset 12
    int32_t imm = 0;         //   lui/auipc/jal (wide) + I-type/load/jalr (12-bit)
    struct {
      int16_t imm16;         //   store offset / branch slot-delta (fits int16)
      uint8_t rs2;           //   R-type / store / branch
    } rb;
  };
};

// alignas(64): pin the base to a cache-line boundary so member offsets map
// deterministically to cache lines. This is load-bearing for *consistent*
// benchmarking (without it base-mod-64 varies per allocation and the same
// layout measures differently run to run) and for the read-mostly / read-write
// field split below to mean anything. Stated explicitly on the class rather
// than relying on the alignas(64) on gas_remaining_ to bump alignof as a side
// effect.
class alignas(64) ThreadedMachine {
public:
  ThreadedMachine(std::shared_ptr<const ThreadedProgram> program,
                  uint32_t memory_bytes, uint64_t gas, uint32_t initial_pc = 0)
    : initial_pc_(initial_pc), program_(std::move(program))
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
    pending_ecall_.reset();
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
  // Yield-based seam: drive the machine without a Host in the dispatch loop.
  // start() runs until the first kEcall/halt/trap; resume_from_ecall() applies
  // the host result and continues. pending_ecall() exposes the stashed args.
  [[nodiscard]] StepResult start();
  [[nodiscard]] StepResult resume_from_ecall(const HostResult &result);
  [[nodiscard]] std::optional<EcallArgs> pending_ecall() const
  {
    return pending_ecall_;
  }
  [[nodiscard]] MachineView view();

  [[nodiscard]] uint32_t reg(size_t i) const
  {
    // regs_[0] is the trash sink (x0 writes); architectural x0 reads as 0.
    return (i != 0 && i < kRegisterCount) ? regs_[i] : 0;
  }
  [[nodiscard]] uint32_t pc() const { return pc_; }
  [[nodiscard]] uint64_t gas_remaining() const { return gas_remaining_; }
  [[nodiscard]] Trap trap() const { return trap_; }
  [[nodiscard]] bool halted() const { return halted_; }
  [[nodiscard]] uint32_t halt_code() const { return halt_code_; }
  [[nodiscard]] std::span<const uint8_t> memory() const { return memory_; }

  // Build (once, then cached) the executable Slot array from the decoded
  // program, baking the traced or untraced handler set per `trace`. Defined
  // out-of-line below, after the handler functions it references.
  void build_exec(bool trace);

private:
  // Internal seam primitives (not part of the public API; the free-function
  // handlers below never call these). drive re-enters the musttail dispatch
  // chain at pc_, returning the first terminal StepResult (kEcall/kHalted/
  // kTrap). apply_ecall is the apply-only complete half of the ecall: it debits
  // host gas and commits the result, but does not run on. Both are defined
  // out-of-line below (they call enter_run/trap_at).
  template<bool Trace> StepResult drive();
  template<bool Trace> StepResult apply_ecall(const HostResult &result);

public:
  // Public so the free-function handlers (below) can manipulate state. This is
  // an experimental, single-translation-unit interpreter; the handlers are
  // effectively part of the machine.
  //
  // regs_ is deliberately the first member (offset 0): the machine pointer is
  // then also the register-file base, so hot-path regs_[i] is a single indexed
  // load with no base adjustment, while every other (cold) field is still
  // reached through the same pointer at its own offset.
  // One extra register. EXPERIMENT(flip): regs_[0] is the x0 write-sink (trash)
  // and regs_[kRegisterCount] is the always-zero register that x0 *reads* remap
  // to, so register writes stay unconditional (no x0 special-case). regs_[0] is
  // garbage during execution; architectural x0 (==0) is restored in
  // reg()/capture()/view(). regs_[kRegisterCount] is never written.
  std::array<uint32_t, kRegisterCount + 1> regs_ = {}; // offset 0: register base
  // Read-only / read-mostly data grouped right after regs_ (regs_[kRegisterCount]
  // is the always-0 x0 read source). max_pc_ (read in op_jalr's valid_target)
  // and the cold pointers go here; the read/WRITE hot scalar gas_remaining_ is
  // pushed to its own cache line below so its per-run-head store doesn't disturb
  // this read-mostly region.
  uint32_t max_pc_ = 0; // program size in bytes; op_jalr bounds-check (build_exec)
  std::vector<uint8_t> memory_;
  std::vector<Slot> exec_;       // size n+1; [n] is the bad-pc sentinel
  TraceObserver *obs_ = nullptr; // set on the traced run(); cold
  // Read/write hot scalar on its own cache line, isolated from the read-mostly
  // data above and from the regs_ reads.
  alignas(64) uint64_t gas_remaining_ = 0; // checked/debited at every run head
  uint32_t pc_ = 0;                        // written per step on the traced path
  // --- cold below ---
  Trap trap_ = Trap::kNone;
  std::optional<EcallArgs>
      pending_ecall_; // set while suspended at an ecall; cold
  uint32_t initial_pc_ = 0;
  Trap construction_trap_ = Trap::kNone;
  bool gas_overflow_ = false; // a run's gas exceeded uint16 -> reject (drive traps)
  bool halted_ = false;
  bool exec_built_ = false;
  bool exec_traced_ = false;
  uint32_t halt_code_ = 0;
  std::shared_ptr<const ThreadedProgram> program_;
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
  std::copy_n(c.m->regs_.begin(), kRegisterCount, s.regs.begin());
  s.regs[0] = 0; // regs_[0] is the trash sink; architectural x0 is 0
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
// The conditional branches (B-type). Their taken target is PC-relative and
// static, so build_exec resolves it the same way as jal.
inline bool
is_conditional_branch(Op op)
{
  switch (op) {
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
    return true;
  default:
    return false;
  }
}

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
    (c).m->gas_remaining_ = gas;                                               \
    (c).m->pc_ = pc_of((c), (next));                                           \
    (c).m->obs_->on_retire(capture(c), StepResult::kContinue);                 \
  }                                                                            \
  HOGD_MUSTTAIL return (next)->handler((c), (next), mem, memsize, gas)

// Tail-dispatch that *starts a new run* at `next` (after a barrier): record the
// retired step (traced), then enter_run charges the new run's gas.
#define HOGD_ENTER_RUN(c, next)                                                \
  if constexpr (Trace) {                                                       \
    (c).m->gas_remaining_ = gas;                                               \
    (c).m->pc_ = pc_of((c), (next));                                           \
    (c).m->obs_->on_retire(capture(c), StepResult::kContinue);                 \
  }                                                                            \
  HOGD_MUSTTAIL return enter_run<Trace>((c), (next), mem, memsize, gas)

// Set a terminal trap at `ip`'s pc, observe it, return kTrap. The trap class is
// a template parameter (compile-time literal at every call site). Its signature
// matches a handler's, so fault sites HOGD_MUSTTAIL into it (a single
// tail-branch, no frame). Spills the register-carried `gas` back to
// gas_remaining_ first, so observable state at the trap is correct.
// cold+noinline: faults are off the hot path, and out-of-lining keeps the pc_of
// divide and the pc_/trap_/halted_ stores out of every handler body.
template<bool Trace, Trap T>
[[gnu::noinline, gnu::cold]] StepResult
trap_at(Ctx c, const Slot *ip, [[maybe_unused]] uint8_t *mem,
        [[maybe_unused]] uint32_t memsize, uint64_t gas)
{
  c.m->gas_remaining_ = gas;
  c.m->pc_ = pc_of(c, ip);
  c.m->trap_ = T;
  c.m->halted_ = true;
  if constexpr (Trace) {
    c.m->obs_->on_retire(capture(c), StepResult::kTrap);
  }
  return StepResult::kTrap;
}

// Charge the gas for the whole run starting at `ip` -- its body through and
// including its terminating barrier -- then dispatch to it. Called at every run
// head: program start, every jump/branch target, and the instruction after each
// barrier. This is the only place gas is charged; no instruction self-charges.
// Block-granular OOG: if the run does not fit, abort at its head with nothing
// debited (total gas to the next barrier is what persists).
template<bool Trace>
StepResult
enter_run(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  const uint32_t run_gas = ip->gas_to_run;
  if (gas < run_gas) {
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kOutOfGas>(c, ip, mem, memsize, gas);
  }
  gas -= run_gas;
  HOGD_MUSTTAIL return ip->handler(c, ip, mem, memsize, gas);
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
op_rr(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = Fn(c.m->regs_[ip->rs1], c.m->regs_[ip->rb.rs2]);
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace, uint32_t (*Fn)(uint32_t, uint32_t)>
StepResult
op_ri(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = Fn(c.m->regs_[ip->rs1], bits_from_i32(ip->imm));
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace>
StepResult
op_lui(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = bits_from_i32(ip->imm);
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace>
StepResult
op_auipc(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = pc_of(c, ip) + bits_from_i32(ip->imm);
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace>
StepResult
op_fence(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
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
  return (target % kInstructionBytes) == 0 && target < c.m->max_pc_;
}

// Branch comparison predicates. Baked into op_branch as a template parameter so
// the hot path runs one comparison directly, instead of a runtime switch on
// ip->op (which lowered to a binary search over the six branch kinds).
inline bool
beq_cmp(uint32_t a, uint32_t b)
{
  return a == b;
}
inline bool
bne_cmp(uint32_t a, uint32_t b)
{
  return a != b;
}
inline bool
blt_cmp(uint32_t a, uint32_t b)
{
  return i32_from_bits(a) < i32_from_bits(b);
}
inline bool
bge_cmp(uint32_t a, uint32_t b)
{
  return i32_from_bits(a) >= i32_from_bits(b);
}
inline bool
bltu_cmp(uint32_t a, uint32_t b)
{
  return a < b;
}
inline bool
bgeu_cmp(uint32_t a, uint32_t b)
{
  return a >= b;
}

// Like jal, a branch's taken target is PC-relative and resolved at decode time:
// in range, imm is rewritten to the target slot delta and op_branch skips the
// alignment/range check; out of range, op_branch_bad_target (below) is baked.
// The not-taken path falls through to ip+1 regardless of the target's validity.
// Cmp is the comparison, fixed per branch kind at decode (handler_for), so the
// hot path is charge -> one compare -> dispatch, with no op-kind switch.
template<bool Trace, bool (*Cmp)(uint32_t, uint32_t)>
StepResult
op_branch(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  if (!Cmp(c.m->regs_[ip->rs1], c.m->regs_[ip->rb.rs2])) {
    HOGD_ENTER_RUN(c, ip + 1);
  }
  const Slot *next = ip + ip->rb.imm16; // imm16 == precomputed target slot delta
  HOGD_ENTER_RUN(c, next);
}

// Baked in place of op_branch when the (static) taken target is out of range. A
// not-taken branch still falls through normally; only a taken one traps kBadPc
// at the branch's own pc. Cmp is baked per branch kind at decode, exactly as in
// op_branch, so the comparison is one direct call, not a runtime switch on op.
template<bool Trace, bool (*Cmp)(uint32_t, uint32_t)>
StepResult
op_branch_bad_target(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  if (!Cmp(c.m->regs_[ip->rs1], c.m->regs_[ip->rb.rs2])) {
    HOGD_ENTER_RUN(c, ip + 1);
  }
  HOGD_MUSTTAIL return trap_at<Trace, Trap::kBadPc>(c, ip, mem, memsize, gas);
}

// jal's target is PC-relative and known at decode time, so build_exec resolves
// it: for an in-range target, `imm` is rewritten to the *slot delta* to the
// target (target_slot - this_slot), and the byte target / alignment / range
// check is hoisted out of the hot path. An out-of-range target instead bakes
// trap_at<kBadPc> directly (build_exec), so this fast path has no validity check.
template<bool Trace>
StepResult
op_jal(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = pc_of(c, ip) + kInstructionBytes;
  const Slot *next = ip + ip->imm; // imm == precomputed target slot delta
  HOGD_ENTER_RUN(c, next);
}

template<bool Trace>
StepResult
op_jalr(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  const uint32_t pc = pc_of(c, ip);
  const uint32_t target =
      (c.m->regs_[ip->rs1] + bits_from_i32(ip->imm)) & ~uint32_t{1};
  if (!valid_target(c, target)) {
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kBadPc>(c, ip, mem, memsize, gas);
  }
  c.m->regs_[ip->rd] = pc + kInstructionBytes;
  const Slot *next = c.base + (target / kInstructionBytes);
  HOGD_ENTER_RUN(c, next);
}

// Width-parameterized little-endian memory helpers. Not shared with
// spec::load_le (the spec namespace is independent test infrastructure) or
// encoding.h's load_u32_le (fixed 4-byte width); a local copy keeps the
// interpreter self-contained and width-generic.
// Single fixed-width access via memcpy, which clang lowers to one ldr/str for a
// constant Width (the byte-shift loop, even unrolled, was left as N byte
// loads). The caller has already alignment- and bounds-checked addr; the host
// is little-endian like RV32, so the bytes map directly.
static_assert(std::endian::native == std::endian::little,
              "le_load/le_store assume a little-endian host");

template<uint32_t Width>
inline uint32_t
le_load(const uint8_t *mem, uint32_t addr)
{
  uint32_t v = 0;
  std::memcpy(&v, mem + addr, Width);
  return v;
}

template<uint32_t Width>
inline void
le_store(uint8_t *mem, uint32_t addr, uint32_t v)
{
  std::memcpy(mem + addr, &v, Width);
}

// Loads/stores are non-barriers: no self-charge (their gas is in the run total
// pre-charged by enter_run), and they continue in-run via THREAD_NEXT. They can
// still fault (misaligned/oob), which is terminal.
template<bool Trace, uint32_t Width, bool SignExtend>
StepResult
op_load(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  const uint32_t addr = c.m->regs_[ip->rs1] + bits_from_i32(ip->imm);
  if ((addr % Width) != 0) {
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kMisalignedAccess>(c, ip, mem,
                                                                 memsize, gas);
  }
  if (!memory_range_in_bounds<Width>(memsize, addr)) {
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kBadMemory>(c, ip, mem, memsize, gas);
  }
  uint32_t v = le_load<Width>(mem, addr);
  if constexpr (SignExtend) {
    v = sign_extend(v, Width * 8);
  }
  c.m->regs_[ip->rd] = v;
  HOGD_THREAD_NEXT(c, ip + 1);
}

template<bool Trace, uint32_t Width>
StepResult
op_store(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  const uint32_t addr = c.m->regs_[ip->rs1] + bits_from_i32(ip->rb.imm16);
  if ((addr % Width) != 0) {
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kMisalignedAccess>(c, ip, mem,
                                                                 memsize, gas);
  }
  if (!memory_range_in_bounds<Width>(memsize, addr)) {
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kBadMemory>(c, ip, mem, memsize, gas);
  }
  le_store<Width>(mem, addr, c.m->regs_[ip->rb.rs2]);
  HOGD_THREAD_NEXT(c, ip + 1);
}

// op_ecall suspends: it advances pc, stashes the args, and yields
// StepResult::kEcall. The ecall's instruction gas was charged at the run head
// (enter_run); on resume, execution re-enters at the post-ecall instruction --
// a fresh run head -- so the ecall's gas is charged exactly once. Because the
// dispatch chain is musttail, returning kEcall unwinds straight back to the
// drive<Trace>() frame. The host call and result handling live in
// apply_ecall<Trace> (the complete half), so the threaded machine is drivable
// from an external scheduler with no Host in the dispatch loop.
template<bool Trace>
StepResult
op_ecall(Ctx c, const Slot *ip, [[maybe_unused]] uint8_t *mem,
         [[maybe_unused]] uint32_t memsize, uint64_t gas)
{
  // Spill the register-carried gas back to the struct: the host sees it via
  // view(), and apply_ecall debits host gas from gas_remaining_. On resume,
  // drive() reloads it into the register.
  c.m->gas_remaining_ = gas;
  // pc advances past the ecall before the host call (canonical ordering).
  const Slot *next = ip + 1;
  c.m->pc_ = pc_of(c, next);
  c.m->pending_ecall_ = EcallArgs{.a0 = c.m->regs_[10],
                                  .a1 = c.m->regs_[11],
                                  .a2 = c.m->regs_[12],
                                  .a3 = c.m->regs_[13],
                                  .a4 = c.m->regs_[14],
                                  .a5 = c.m->regs_[15],
                                  .a6 = c.m->regs_[16],
                                  .a7 = c.m->regs_[17]};
  return StepResult::kEcall;
}

// Sequential fall-off past the last instruction (the exec_[n] sentinel) and
// out-of-range jal/jalr both trap kBadPc; the unsupported-op default traps
// kInvalidInstruction. All three bake trap_at<...> directly into the Slot
// rather than going through a forwarding handler (see handler_for/build_exec).

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
    return &op_branch<Trace, beq_cmp>;
  case Op::kBne:
    return &op_branch<Trace, bne_cmp>;
  case Op::kBlt:
    return &op_branch<Trace, blt_cmp>;
  case Op::kBge:
    return &op_branch<Trace, bge_cmp>;
  case Op::kBltu:
    return &op_branch<Trace, bltu_cmp>;
  case Op::kBgeu:
    return &op_branch<Trace, bgeu_cmp>;
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
    // ebreak's gas is charged at the run head; the handler is a pure trap, so
    // dispatch straight to trap_at (it has the handler signature).
    return &trap_at<Trace, Trap::kBreakpoint>;
  case Op::kEcall:
    return &op_ecall<Trace>;
  default:
    // Unreachable by construction (ThreadedProgram::create rejects unsupported
    // ops); the defensive default traps kInvalidInstruction directly.
    return &trap_at<Trace, Trap::kInvalidInstruction>;
  }
}

// Selects the out-of-range branch handler with the same Cmp handler_for bakes
// for the in-range case. Build-time only (cold); build_exec calls it solely for
// is_conditional_branch ops, so a non-branch op is a build_exec bug -- assert
// and abort rather than mislabel it as a guest-visible invalid instruction.
template<bool Trace>
Handler
branch_bad_target_handler_for(Op op)
{
  switch (op) {
  case Op::kBeq:
    return &op_branch_bad_target<Trace, beq_cmp>;
  case Op::kBne:
    return &op_branch_bad_target<Trace, bne_cmp>;
  case Op::kBlt:
    return &op_branch_bad_target<Trace, blt_cmp>;
  case Op::kBge:
    return &op_branch_bad_target<Trace, bge_cmp>;
  case Op::kBltu:
    return &op_branch_bad_target<Trace, bltu_cmp>;
  case Op::kBgeu:
    return &op_branch_bad_target<Trace, bgeu_cmp>;
  default:
    assert(false && "branch_bad_target_handler_for: op is not a conditional "
                    "branch");
    std::abort();
  }
}

// True for ops whose Slot uses the {imm16, rs2} union arm because the handler
// reads rs2: R-type ALU, stores, conditional branches. Everyone else uses the
// wide `imm` arm and never reads rs2.
inline bool
slot_packs_rs2(Op op)
{
  switch (op) {
  case Op::kBeq:
  case Op::kBne:
  case Op::kBlt:
  case Op::kBge:
  case Op::kBltu:
  case Op::kBgeu:
  case Op::kSb:
  case Op::kSh:
  case Op::kSw:
  case Op::kAdd:
  case Op::kSub:
  case Op::kSll:
  case Op::kSlt:
  case Op::kSltu:
  case Op::kXor:
  case Op::kSrl:
  case Op::kSra:
  case Op::kOr:
  case Op::kAnd:
  case Op::kMul:
  case Op::kMulh:
  case Op::kMulhsu:
  case Op::kMulhu:
  case Op::kDiv:
  case Op::kDivu:
  case Op::kRem:
  case Op::kRemu:
    return true;
  default:
    return false;
  }
}

// Guards the "this immediate fits the 16-bit imm16 arm" assumption (debug
// only). True for every store offset (12-bit) and in-range branch slot-delta
// (byte offset / 4, <= +/-1024); a violation means a decoder/packing bug.
constexpr bool
fits_int16(int32_t v)
{
  return v >= std::numeric_limits<int16_t>::min() &&
         v <= std::numeric_limits<int16_t>::max();
}

inline void
ThreadedMachine::build_exec(bool trace)
{
  const std::span<const Instr> code = program_->code();
  const size_t n = code.size();
  exec_.resize(n + 1);
  for (size_t i = 0; i < n; ++i) {
    const Instr &d = code[i];
    Slot &slot = exec_[i];
    slot.handler =
        trace ? handler_for<true>(d.op) : handler_for<false>(d.op);
    slot.gas_to_run = 0; // filled by the backward pass below
    // EXPERIMENT(flip): x0 writes land in the trash at regs_[0] (rd unmapped),
    // x0 reads come from the always-zero register at regs_[kRegisterCount] (rs
    // remapped). Observable state forces x0 (regs_[0]) back to 0.
    slot.rd = static_cast<uint8_t>(d.rd);
    slot.rs1 = (d.rs1 == 0) ? static_cast<uint8_t>(kRegisterCount)
                            : static_cast<uint8_t>(d.rs1);
    // Pick the union arm: rs2-users get {imm16, rs2}; everyone else the wide
    // imm. (imm16 is the store offset here; for branches it's overwritten with
    // the slot delta below; R-type ignores it.)
    if (slot_packs_rs2(d.op)) {
      slot.rb.rs2 = (d.rs2 == 0) ? static_cast<uint8_t>(kRegisterCount)
                                 : static_cast<uint8_t>(d.rs2);
      assert(fits_int16(d.imm) && "store immediate must fit imm16");
      slot.rb.imm16 = static_cast<int16_t>(d.imm);
    }
    else {
      slot.imm = d.imm;
    }
    // Resolve the PC-relative target of jal and the conditional branches once,
    // here, so their handlers need no per-execution alignment/range check. In
    // range: rewrite imm to the target slot delta (exact -- a valid target is
    // 4-aligned, so the byte imm is a multiple of kInstructionBytes). jal keeps
    // the wide imm arm (its slot delta can exceed int16); a branch's fits int16.
    // Out of range: swap in the op-specific trapping handler.
    if (d.op == Op::kJal || is_conditional_branch(d.op)) {
      const uint32_t pc = static_cast<uint32_t>(i) * kInstructionBytes;
      const uint32_t target = pc + bits_from_i32(d.imm);
      if (program_->contains_pc(target)) {
        const int32_t delta = d.imm / static_cast<int32_t>(kInstructionBytes);
        if (d.op == Op::kJal) {
          slot.imm = delta;
        }
        else {
          assert(fits_int16(delta) && "branch slot-delta must fit imm16");
          slot.rb.imm16 = static_cast<int16_t>(delta);
        }
      }
      else if (d.op == Op::kJal) {
        // Out-of-range jal: a taken jal always traps, so bake trap_at directly
        // (gas was charged at the run head, matching the in-range ordering).
        slot.handler = trace ? &trap_at<true, Trap::kBadPc>
                             : &trap_at<false, Trap::kBadPc>;
      }
      else {
        slot.handler = trace ? branch_bad_target_handler_for<true>(d.op)
                             : branch_bad_target_handler_for<false>(d.op);
      }
    }
  }
  // Sentinel for sequential fall-off past the last instruction: trap kBadPc at
  // the out-of-range pc (its pc is (ip - base) * 4). No gas is charged.
  exec_[n] = Slot{.handler = trace ? &trap_at<true, Trap::kBadPc>
                                   : &trap_at<false, Trap::kBadPc>};
  // gas_to_run[i] = gas of the run starting at i, through and including its
  // terminating barrier; backward so each entry sees its successor's
  // already-computed sum. enter_run pre-charges this at every run head, so no
  // instruction self-charges. A barrier ends its run, so its gas_to_run is just
  // its own gas (charged when a jump/branch lands directly on it); a
  // non-barrier always continues into its successor, and the sum stops
  // naturally at the barrier (whose gas_to_run excludes its own successor). The
  // i+1<n guard keeps the run from absorbing the fall-off sentinel (exec_[n],
  // gas 0). The per-instruction op and gas come straight from `code` -- they
  // are build-time inputs, not stored in the Slot.
  //
  // gas_to_run is a uint16. A run whose total gas exceeds 65535 (only reachable
  // by a pathologically long straight-line run -- up to 16384 instrs * 8 gas
  // for div) is rejected: gas_overflow_ makes drive() trap. TODO: instead split
  // the run, charging 65535 at a synthetic mid-run recharge handler.
  bool overflow = false;
  for (size_t i = n; i-- > 0;) {
    const auto self_gas = static_cast<uint32_t>(code[i].gas);
    const uint32_t run_gas =
        is_barrier(code[i].op)
            ? self_gas
            : self_gas + ((i + 1 < n) ? exec_[i + 1].gas_to_run : 0U);
    overflow = overflow || run_gas > 0xFFFFU;
    exec_[i].gas_to_run = static_cast<uint16_t>(run_gas);
  }
  gas_overflow_ = overflow;
  max_pc_ = static_cast<uint32_t>(n) * kInstructionBytes;
  exec_built_ = true;
  exec_traced_ = trace;
}

inline MachineView
ThreadedMachine::view()
{
  regs_[0] = 0; // regs_[0] is the x0 trash sink; present architectural 0
  return MachineView(
      pc_, gas_remaining_,
      std::span<const uint32_t, kRegisterCount>(regs_.data(), kRegisterCount),
      memory_);
}

template<bool Trace>
StepResult
ThreadedMachine::drive()
{
  // Reject a program whose run gas overflowed uint16 (set in build_exec).
  // TODO(gramseyer): split overflowing runs instead of rejecting.
  if (gas_overflow_) {
    trap_ = Trap::kInvalidInstruction;
    halted_ = true;
    return StepResult::kTrap;
  }
  Ctx c{.m = this, .base = exec_.data()};
  const Slot *ip = c.base + (pc_ / kInstructionBytes);
  uint8_t *mem = memory_.data();
  const auto memsize = static_cast<uint32_t>(memory_.size());
  // Plain call+return, not a musttail: a non-static member function cannot
  // tail-call the free function enter_run (musttail needs matching signatures,
  // and the implicit `this` differs). enter_run begins the musttail chain,
  // which runs in O(1) stack; this is the single non-recursive bridge frame
  // into it. Seed the register-carried gas from the struct; the dispatch chain
  // carries it onward and spills it back at terminal/observe/ecall points.
  return enter_run<Trace>(c, ip, mem, memsize, gas_remaining_);
}

// Apply-only complete half: debit host gas, commit the result, and (on the
// traced path) emit the ecall's retirement exactly as the inline op_ecall did.
// Does NOT run on -- the caller continues via drive<Trace>().
template<bool Trace>
StepResult
ThreadedMachine::apply_ecall(const HostResult &result)
{
  assert(pending_ecall_.has_value());
  pending_ecall_.reset();

  // pc_ is already pc_after (set in op_ecall); rebuild Ctx/next for trap_at and
  // capture(). next's pc == pc_after, matching the inline trap_at(..., next).
  Ctx c{.m = this, .base = exec_.data()};
  const Slot *next = c.base + (pc_ / kInstructionBytes);
  uint8_t *mem = memory_.data();
  const auto memsize = static_cast<uint32_t>(memory_.size());

  // Debit host gas before committing; over-budget host cannot mutate state.
  // Host memory writes persist on OOG (no per-ecall rollback).
  if (gas_remaining_ < result.gas_consumed) {
    return trap_at<Trace, Trap::kOutOfGas>(c, next, mem, memsize, gas_remaining_);
  }
  gas_remaining_ -= result.gas_consumed;

  switch (result.status) {
  case HostStatus::kContinue:
    regs_[10] = result.continue_value();
    if constexpr (Trace) {
      // The ecall's retirement (inline path records it via HOGD_ENTER_RUN).
      obs_->on_retire(capture(c), StepResult::kContinue);
    }
    return StepResult::kContinue;
  case HostStatus::kHalt:
    halt_code_ = result.halt_code();
    halted_ = true;
    if constexpr (Trace) {
      obs_->on_retire(capture(c), StepResult::kHalted);
    }
    return StepResult::kHalted;
  case HostStatus::kOutOfGas:
    return trap_at<Trace, Trap::kOutOfGas>(c, next, mem, memsize, gas_remaining_);
  case HostStatus::kTrap:
  default:
    return trap_at<Trace, Trap::kHostTrap>(c, next, mem, memsize, gas_remaining_);
  }
}

inline StepResult
ThreadedMachine::start()
{
  if (halted_) {
    return trap_ == Trap::kNone ? StepResult::kHalted : StepResult::kTrap;
  }
  // Misuse to start with an outstanding ecall: drive<false> would execute past
  // the ecall without applying the host result.
  assert(!pending_ecall_.has_value());
  if (!exec_built_ || exec_traced_) {
    build_exec(/*trace=*/false);
  }
  obs_ = nullptr;
  return drive<false>();
}

inline StepResult
ThreadedMachine::resume_from_ecall(const HostResult &result)
{
  const StepResult applied = apply_ecall<false>(result);
  return applied == StepResult::kContinue ? drive<false>() : applied;
}

inline StepResult
ThreadedMachine::run(Host &host)
{
  // Thin synchronous wrapper over the untraced yield API.
  StepResult result = start();
  while (result == StepResult::kEcall) {
    assert(pending_ecall_.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    result = resume_from_ecall(host.ecall(view(), *pending_ecall_));
    // NOLINTEND(bugprone-unchecked-optional-access)
  }
  return result;
}

inline StepResult
ThreadedMachine::run(Host &host, TraceObserver &obs)
{
  if (halted_) {
    return trap_ == Trap::kNone ? StepResult::kHalted : StepResult::kTrap;
  }
  // Same precondition as start(): a fresh run must not begin with an
  // outstanding ecall (drive<true> would execute past it without applying the
  // host result). run(Host&) gets this via start(); the traced path asserts it
  // directly since it drives the chain itself.
  assert(!pending_ecall_.has_value());
  if (!exec_built_ || !exec_traced_) {
    build_exec(/*trace=*/true);
  }
  obs_ = &obs;
  obs.on_initial(capture(Ctx{.m = this, .base = exec_.data()}));
  StepResult result = drive<true>();
  while (result == StepResult::kEcall) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    assert(pending_ecall_.has_value());
    const StepResult applied =
        apply_ecall<true>(host.ecall(view(), *pending_ecall_));
    // NOLINTEND(bugprone-unchecked-optional-access)
    if (applied != StepResult::kContinue) {
      result = applied;
      break;
    }
    result = drive<true>();
  }
  return result;
}

} // namespace hogd::rv32
