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
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/arch_trace.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/isa_ops.h"
#include "hogd/hog_exec/runtime/rv32/musttail.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/threaded_program.h"

// Direct-threaded RV32IM interpreter. Handlers tail-call the next Slot; the
// guest PC is derived from the slot address at control-flow and observation
// boundaries.
//
// enter_run() charges a straight-line run, including its terminating barrier
// or chunk-break instruction. A memory fault refunds the unexecuted suffix.
// An unaffordable run executes speculatively with its shortfall recorded in
// gas_deficit_; run exits settle it before another run or host call can start.
// Trap classification and remaining gas match the canonical Machine. After
// out-of-gas, speculative registers, memory, trap PC, and intermediate traces
// may differ; the failed invocation discards those effects.
//
// Preparation selects traced or untraced handlers. Traced handlers record each
// retired instruction, including both instructions of a fused pair.
namespace hogd::rv32 {

struct Ctx;
struct Slot;
using Handler = StepResult (*)(Ctx, const Slot *, uint8_t *, uint32_t,
                               uint64_t);

// Chunk limit: bounds gas_to_run's uint16 value and the speculative deficit.
// Lowering splits longer runs; every individual instruction must fit.
inline constexpr uint32_t kMaxRunGas = 10000;
static_assert(kMaxInstructionGas <= kMaxRunGas);
static_assert(kMaxRunGas <= UINT16_MAX);

// Named outside the anonymous union because ISO C++ forbids defining a type
// inside one. Instructions using rs2 need at most a 16-bit immediate.
struct RbOperands {
  int16_t imm16; // store offset / branch slot-delta (fits int16)
  uint8_t rs2;   // R-type / store / branch
};

// Power-of-two slot size makes PC and branch-target arithmetic shifts.
// x0 writes go to regs_[0]; x0 reads are remapped to regs_[kRegisterCount],
// which stays zero. Observers restore architectural x0.
//
// The operand union shares rs2 with the immediate: lui/auipc/jal need a wide
// immediate but no rs2; R-type/store/branch need rs2 and at most int16.
// gas_to_run holds the suffix cost through the next barrier or chunk break,
// allowing any slot to become a run head after a branch.
struct alignas(16) Slot {
  Handler handler = nullptr; // offset 0
  uint16_t gas_to_run = 0;   // offset 8
  uint8_t rd = 0;            // offset 10: dest reg index (0 -> kTrashReg)
  uint8_t rs1 = 0;           // offset 11
  union {                    // offset 12
    int32_t imm = 0; //   lui/auipc/jal (wide) + I-type/load/jalr (12-bit)
    RbOperands rb;   //   R-type / store / branch: {imm16, rs2}
  };
};
static_assert(sizeof(Slot) == 16);
static_assert(alignof(Slot) == 16);
// Immutable, decode-once form of a contract: the baked Slot handler array plus
// the initial memory image (the contract's seeded bytes zero-padded out to
// total_memory_size). Shared across ThreadedMachine instantiations -- each
// machine copies the full image into its own memory_ at construction/reset.
class ThreadedMachinePreInstantiation {
public:
  // `memory` is the initial seeded image (e.g. an artifact's initial_memory);
  // it is placed at data-space VA `memory_start`, with zeros filling [0,
  // memory_start) and [memory_start+memory.size(), total_memory_size).
  // `total_memory_size` is the machine's full memory size and must be <=
  // kMemoryCeilingBytes; memory_start + memory.size() must be <=
  // total_memory_size.
  // `max_run_gas` is the chunk cap the lowering splits runs at; production
  // always uses the default kMaxRunGas. Tests and the engine-diff fuzzer pass
  // smaller caps so break carriers (and their refund/deficit seams) appear in
  // short programs instead of requiring >kMaxRunGas of straight-line gas. It
  // is clamped to [kMaxInstructionGas, kMaxRunGas]: the floor keeps a valid
  // split for every single instruction, the ceiling keeps gas_to_run within
  // its uint16 field and the deficit overdraft bound.
  [[nodiscard]] static std::expected<ThreadedMachinePreInstantiation,
                                     ProgramError>
  create(std::span<const uint8_t> code, std::span<const uint8_t> memory,
         uint32_t memory_start, uint32_t total_memory_size, bool trace,
         uint32_t max_run_gas = kMaxRunGas, bool fuse = true);

  ThreadedMachinePreInstantiation(ThreadedMachinePreInstantiation &&) noexcept =
      default;
  ThreadedMachinePreInstantiation &
  operator=(ThreadedMachinePreInstantiation &&) noexcept = default;
  ThreadedMachinePreInstantiation(const ThreadedMachinePreInstantiation &) =
      delete;
  ThreadedMachinePreInstantiation &
  operator=(const ThreadedMachinePreInstantiation &) = delete;

private:
  ThreadedMachinePreInstantiation(std::vector<Slot> exec,
                                  std::vector<uint8_t> initial_memory_image,
                                  bool trace)
    : exec_(std::move(exec)),
      initial_memory_image_(std::move(initial_memory_image)), trace_(trace)
  {}

  std::vector<Slot> exec_;
  std::vector<uint8_t> initial_memory_image_;
  bool trace_ = false;

  friend struct ThreadedMachinePreInstantiationTestPeer;
  friend class ThreadedMachine;
};

// Recycle released guest-memory buffers per thread. Each live invocation owns
// its buffer, including nested invocations. The cap limits retained buffers;
// deeper nesting allocates normally. Reserve the default memory size up front;
// larger admitted images grow the buffer on first use.
inline constexpr size_t kMemorySlotPoolCap = 8;

// Fixed capacity keeps pool construction and buffer release non-allocating.
struct MemorySlotPool {
  std::array<std::vector<uint8_t>, kMemorySlotPoolCap> slots;
  size_t count = 0;
};

inline MemorySlotPool &
memory_slot_pool() noexcept
{
  static thread_local MemorySlotPool pool;
  return pool;
}

[[nodiscard]] inline std::vector<uint8_t>
acquire_memory_slot()
{
  auto &pool = memory_slot_pool();
  std::vector<uint8_t> slot;
  if (pool.count != 0) {
    slot = std::move(pool.slots[--pool.count]);
  }
  slot.reserve(kDefaultMaxMemoryBytes);
  return slot;
}

inline void
release_memory_slot(std::vector<uint8_t> &&slot) noexcept
{
  auto &pool = memory_slot_pool();
  if (pool.count < kMemorySlotPoolCap) {
    pool.slots[pool.count++] = std::move(slot);
  }
}

// Owns a guest-memory buffer across invocation suspension and moves. Acquisition
// uses the current thread's pool; destruction returns the buffer to the pool
// of the thread running the destructor. No reference to the acquiring pool is
// retained.
//
// capacity() != 0 denotes ownership. Moves explicitly empty the source rather
// than relying on std::vector's unspecified moved-from state. Keeping only the
// vector preserves ThreadedMachine's cache-line layout.
class MemorySlot {
public:
  MemorySlot() : buf_(acquire_memory_slot()) {}
  ~MemorySlot()
  {
    // capacity() != 0 means this slot still holds a pool buffer to return; a
    // moved-from slot was reset to no allocation and releases nothing.
    if (buf_.capacity() != 0) {
      release_memory_slot(std::move(buf_));
    }
  }
  MemorySlot(MemorySlot &&other) noexcept
    : buf_(std::exchange(other.buf_, std::vector<uint8_t>{}))
  {
  }
  MemorySlot &operator=(MemorySlot &&) = delete;
  MemorySlot(const MemorySlot &) = delete;
  MemorySlot &operator=(const MemorySlot &) = delete;

  [[nodiscard]] std::vector<uint8_t> &bytes() noexcept { return buf_; }
  [[nodiscard]] const std::vector<uint8_t> &bytes() const noexcept
  {
    return buf_;
  }

private:
  std::vector<uint8_t> buf_;
};

// Align the machine so member offsets consistently separate the register file,
// read-mostly fields, and gas balance into cache lines.
class alignas(64) ThreadedMachine {
public:
  ThreadedMachine(ThreadedMachinePreInstantiation const &pre_instantiation,
                  uint64_t gas)
    : pre_instantiation_(&pre_instantiation),
      exec_traced_(pre_instantiation.trace_)
  {
    const auto &image = pre_instantiation.initial_memory_image_;
    assert(image.size() <= kMemoryCeilingBytes);
    // memory_ acquired its slot from the pool on construction. The recycled
    // slot is dirty, but the image is full-size (zero-padded to
    // total_memory_size), so this assign fully overwrites it -- and the
    // reserved capacity means no allocation on a pool hit.
    memory_.bytes().assign(image.begin(), image.end());

    regs_.fill(0);
    // Entry ABI v3: sp (x2) stays 0 from the fill above -- the guest owns
    // stack placement (matches canonical Machine::reset).

    exec_ = std::span(pre_instantiation.exec_);
    max_pc_ = (exec_.size() - 1) * kInstructionBytes;
    gas_remaining_ = gas;
  }

  ThreadedMachine(ThreadedMachine &&) noexcept = default;

  // exec_ borrows the pre-instantiation's Slot array (not owned), so the
  // pre-instantiation must outlive the machine. Reject binding a temporary,
  // which would leave exec_ dangling the moment the constructor returns.
  ThreadedMachine(ThreadedMachinePreInstantiation &&, uint64_t) = delete;

  void reset(uint64_t gas)
  {
    regs_.fill(0);
    // memory_ is sized to the (already zero-padded) image, so restoring it is a
    // straight copy -- no separate tail zero-fill needed.
    std::ranges::copy(pre_instantiation_->initial_memory_image_,
                      memory_.bytes().begin());
    pc_ = 0;
    gas_remaining_ = gas;
    gas_deficit_ = 0;
    trap_ = Trap::kNone;
    halted_ = false;
    halt_code_ = 0;
    pending_ecall_.reset();
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

  // Seed the method ID for guest dispatch. Call after construction/reset and
  // before start()/run().
  void seed_method_id(uint64_t method_id)
  {
    regs_[10] = static_cast<uint32_t>(method_id);       // a0 = low 32 bits
    regs_[11] = static_cast<uint32_t>(method_id >> 32); // a1 = high 32 bits
  }

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
  [[nodiscard]] std::span<uint8_t> memory() { return memory_.bytes(); }

  [[nodiscard]] bool consume_gas(uint64_t amount)
  {
    if (amount > gas_remaining_) {
      gas_remaining_ = 0;
      return false;
    }
    gas_remaining_ -= amount;
    return true;
  }

private:
  // drive resumes dispatch at pc_; apply_ecall debits host gas and applies the
  // result without resuming dispatch.
  template<bool Trace> StepResult drive();
  template<bool Trace> StepResult apply_ecall(const HostResult &result);

public:
  // Public for the free-function handlers. Keep regs_ first so the machine
  // pointer is also the register-file base. regs_[0] absorbs x0 writes;
  // regs_[kRegisterCount] supplies x0 reads and is never written.
  std::array<uint32_t, kRegisterCount + 1> regs_ =
      {}; // offset 0: register base
  // Group read-mostly fields separately from the gas balance below.
  uint32_t max_pc_ = 0; // program size in bytes; op_jalr bounds-check
                        // (PreInstantiation::create)
  MemorySlot memory_; // owns this machine's guest-memory pool slot (see above)
  std::span<const Slot> exec_;   // size n+1; [n] is the bad-pc sentinel
  TraceObserver *obs_ = nullptr; // set on the traced run(); cold
  ThreadedMachinePreInstantiation const *pre_instantiation_ = nullptr;
  // Read/write hot scalar on its own cache line, isolated from the read-mostly
  // data above and from the regs_ reads.
  alignas(64) uint64_t gas_remaining_ = 0; // checked/debited at every run head
  uint32_t pc_ = 0; // written per step on the traced path
  // --- cold below ---
  Trap trap_ = Trap::kNone;
  std::optional<EcallArgs> pending_ecall_ =
      std::nullopt; // set while suspended at an ecall; cold
  bool halted_ = false;
  const bool exec_traced_;
  uint32_t halt_code_ = 0;
  // Unsettled overdraft of the (single) speculative run: enter_run_deficit
  // records the shortfall here and runs the body on a zero balance; every run
  // exit seam settles a nonzero value as (kOutOfGas, 0). Kept out-of-band so
  // the uint64 balance keeps its full domain (callers pass UINT64_MAX
  // budgets); bounded by the lowering's kMaxRunGas chunk cap. Cold: written
  // only on the deficit path, read at seams.
  uint32_t gas_deficit_ = 0;
};

// MemorySlot must remain the size of a vector to preserve this layout.
static_assert(offsetof(ThreadedMachine, gas_remaining_) == 192,
              "gas_remaining_ must stay on its own cache line (offset 3 * 64); "
              "check the memory_ member width and field order above");
static_assert(sizeof(ThreadedMachine) == 256,
              "ThreadedMachine size changed -- check the cache-line layout");

// Passed by value: both pointers fit in argument registers on AArch64.
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
  s.memory.assign(c.m->memory_.bytes().begin(), c.m->memory_.bytes().end());
  s.gas_remaining = c.m->gas_remaining_;
  s.trap = c.m->trap_;
  s.halted = c.m->halted_;
  s.halt_code = c.m->halt_code_;
  return s;
}

// Conditional branch targets are static and resolved during preparation.
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

// Barriers end a run and are included in its charge. Loads/stores stay in
// runs; a fault refunds their unexecuted suffix.
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
  case Op::kUnimp:
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
  do {                                                                         \
    if constexpr (Trace) {                                                     \
      (c).m->gas_remaining_ = gas;                                             \
      (c).m->pc_ = pc_of((c), (next));                                         \
      (c).m->obs_->on_retire(capture(c), StepResult::kContinue);               \
    }                                                                          \
    HOGD_MUSTTAIL return (next)->handler((c), (next), mem, memsize, gas);      \
  } while (false)

// Tail-dispatch that *starts a new run* at `next` (after a barrier): record the
// retired step (traced), then enter_run charges the new run's gas.
#define HOGD_ENTER_RUN(c, next)                                                \
  do {                                                                         \
    if constexpr (Trace) {                                                     \
      (c).m->gas_remaining_ = gas;                                             \
      (c).m->pc_ = pc_of((c), (next));                                         \
      (c).m->obs_->on_retire(capture(c), StepResult::kContinue);               \
    }                                                                          \
    HOGD_MUSTTAIL return enter_run<Trace>((c), (next), mem, memsize, gas);     \
  } while (false)

// Continue after a non-barrier instruction. `Brk` (the enclosing handler's
// template parameter) is false for the common in-run step (thread to the next
// slot, no gas work) and true only at a mid-run split point the lowering
// inserted to keep a run's gas within kMaxRunGas: there the slot ends its
// (sub-)run, so control enters a fresh run at the successor, charging the next
// chunk. `if constexpr` keeps Brk==false byte-identical to a bare
// HOGD_THREAD_NEXT (zero hot-path cost); the Brk==true instantiations are cold,
// baked only into the rare split slots.
#define HOGD_STEP_NEXT(c, next)                                                \
  do {                                                                         \
    if constexpr (Brk) {                                                       \
      HOGD_ENTER_RUN((c), (next));                                             \
    }                                                                          \
    else {                                                                     \
      HOGD_THREAD_NEXT((c), (next));                                           \
    }                                                                          \
  } while (false)

// A deficit takes precedence over the requested trap: canonical execution ran
// out of gas first. trap_bad_memory refunds the suffix before reaching here
// so an affordable memory fault retains kBadMemory.
// Keep terminal state materialization out of the hot handlers.
template<bool Trace, Trap T>
[[gnu::noinline, gnu::cold]] StepResult
trap_at(Ctx c, const Slot *ip, [[maybe_unused]] uint8_t *mem,
        [[maybe_unused]] uint32_t memsize, uint64_t gas)
{
  if (c.m->gas_deficit_ != 0) {
    c.m->gas_deficit_ = 0;
    c.m->gas_remaining_ = 0;
    c.m->trap_ = Trap::kOutOfGas;
  }
  else {
    c.m->gas_remaining_ = gas;
    c.m->trap_ = T;
  }
  c.m->pc_ = pc_of(c, ip);
  c.m->halted_ = true;
  if constexpr (Trace) {
    c.m->obs_->on_retire(capture(c), StepResult::kTrap);
  }
  return StepResult::kTrap;
}

// Refund the charged suffix after a memory fault. If the refund covers any
// deficit, canonical execution could afford the faulting instruction; report
// kBadMemory with that prefix charged. Otherwise report kOutOfGas with zero.
// A break carrier has no charged suffix: ip+1 belongs to the next chunk.
template<bool Trace, bool Brk = false>
StepResult
trap_bad_memory(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize,
                uint64_t gas)
{
  // No overflow: a paid suffix refund cannot exceed the entry balance;
  // in a deficit run, gas is zero and the suffix is bounded by kMaxRunGas.
  const uint64_t suffix = Brk ? uint64_t{0} : uint64_t{(ip + 1)->gas_to_run};
  const uint64_t balance = gas + suffix;
  const uint32_t deficit = c.m->gas_deficit_;
  if (balance < deficit) {
    // trap_at's deficit branch settles this as (kOutOfGas, 0).
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kOutOfGas>(c, ip, mem, memsize,
                                                         /*gas=*/0);
  }
  c.m->gas_deficit_ = 0;
  HOGD_MUSTTAIL return trap_at<Trace, Trap::kBadMemory>(c, ip, mem, memsize,
                                                        balance - deficit);
}

// Only one run may execute in deficit. A second run reaches this path with
// zero gas and settles the prior deficit as kOutOfGas. Ecall and trap exits
// settle it separately. Tracking the shortfall outside the balance preserves
// the full uint64 budget range; kMaxRunGas bounds the shortfall.
template<bool Trace>
[[gnu::noinline, gnu::cold]] StepResult
enter_run_deficit(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize,
                  uint64_t gas)
{
  if (c.m->gas_deficit_ != 0) {
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kOutOfGas>(c, ip, mem, memsize,
                                                         /*gas=*/0);
  }
  c.m->gas_deficit_ = ip->gas_to_run - static_cast<uint32_t>(gas);
  HOGD_MUSTTAIL return ip->handler(c, ip, mem, memsize, /*gas=*/0);
}

// Charge through the terminating barrier or chunk break. Called at entry,
// control-flow targets, and after barriers and chunk breaks. An unaffordable
// run takes the speculative path above.
template<bool Trace>
StepResult
enter_run(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  const uint32_t run_gas = ip->gas_to_run;
  if (gas < run_gas) [[unlikely]] {
    HOGD_MUSTTAIL return enter_run_deficit<Trace>(c, ip, mem, memsize, gas);
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
// --- Non-barrier handlers: no gas work (the run was pre-charged) ------------

template<bool Trace, uint32_t (*Fn)(uint32_t, uint32_t), bool Brk = false>
StepResult
op_rr(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = Fn(c.m->regs_[ip->rs1], c.m->regs_[ip->rb.rs2]);
  HOGD_STEP_NEXT(c, ip + 1);
}

template<bool Trace, uint32_t (*Fn)(uint32_t, uint32_t), bool Brk = false>
StepResult
op_ri(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = Fn(c.m->regs_[ip->rs1], bits_from_i32(ip->imm));
  HOGD_STEP_NEXT(c, ip + 1);
}

template<bool Trace, bool Brk = false>
StepResult
op_lui(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = bits_from_i32(ip->imm);
  HOGD_STEP_NEXT(c, ip + 1);
}

template<bool Trace, bool Brk = false>
StepResult
op_auipc(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  c.m->regs_[ip->rd] = pc_of(c, ip) + bits_from_i32(ip->imm);
  HOGD_STEP_NEXT(c, ip + 1);
}

template<bool Trace, bool Brk = false>
StepResult
op_fence(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  HOGD_STEP_NEXT(c, ip + 1);
}

// Barrier handlers end the current run; its gas was charged at entry.

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
  const Slot *next =
      ip + ip->rb.imm16; // imm16 == precomputed target slot delta
  HOGD_ENTER_RUN(c, next);
}

// Baked in place of op_branch when the (static) taken target is out of range. A
// not-taken branch still falls through normally; only a taken one traps kBadPc
// at the branch's own pc. Cmp is baked per branch kind at decode, exactly as in
// op_branch, so the comparison is one direct call, not a runtime switch on op.
template<bool Trace, bool (*Cmp)(uint32_t, uint32_t)>
StepResult
op_branch_bad_target(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize,
                     uint64_t gas)
{
  if (!Cmp(c.m->regs_[ip->rs1], c.m->regs_[ip->rb.rs2])) {
    HOGD_ENTER_RUN(c, ip + 1);
  }
  HOGD_MUSTTAIL return trap_at<Trace, Trap::kBadPc>(c, ip, mem, memsize, gas);
}

// jal's target is PC-relative and known at decode time, so
// PreInstantiation::create resolves it: for an in-range target, `imm` is
// rewritten to the *slot delta* to the target (target_slot - this_slot), and
// the byte target / alignment / range check is hoisted out of the hot path. An
// out-of-range target instead bakes trap_at<kBadPc> directly
// (PreInstantiation::create), so this fast path has no validity check.
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
// loads). The caller has already bounds-checked addr; the host
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

// Consensus-relevant load/store core, factored out of the nine handler bodies
// (plain + fused) that used to repeat it inline. Compute addr = base + imm,
// bounds-check it for Width, and on success perform the access; return false
// (touching nothing) on an out-of-bounds address so the handler can
// HOGD_MUSTTAIL into trap_at at the *faulting slot's* pc. always_inline: this
// is the hot memory path, so the helper folds into the handler and the trap
// tail-call stays in the handler body (no frame growth). try_load yields the
// loaded (sign-extended if requested) value via `out` rather than writing a
// register, so the fused load handlers can feed it straight into the next op
// without a regs_ round trip; the plain handlers write regs_[rd] = out.
template<uint32_t Width, bool SignExtend>
[[gnu::always_inline]] inline bool
try_load(const uint8_t *mem, uint32_t memsize, uint32_t base, int32_t imm,
         uint32_t &out)
{
  const uint32_t addr = base + bits_from_i32(imm);
  if (!memory_range_in_bounds<Width>(memsize, addr)) {
    return false;
  }
  uint32_t v = le_load<Width>(mem, addr);
  if constexpr (SignExtend) {
    v = sign_extend<Width * 8>(v);
  }
  out = v;
  return true;
}

template<uint32_t Width>
[[gnu::always_inline]] inline bool
try_store(uint8_t *mem, uint32_t memsize, uint32_t base, int32_t imm,
          uint32_t value)
{
  const uint32_t addr = base + bits_from_i32(imm);
  if (!memory_range_in_bounds<Width>(memsize, addr)) {
    return false;
  }
  le_store<Width>(mem, addr, value);
  return true;
}

template<bool Trace, uint32_t Width, bool SignExtend, bool Brk = false>
StepResult
op_load(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  uint32_t v;
  if (!try_load<Width, SignExtend>(mem, memsize, c.m->regs_[ip->rs1], ip->imm,
                                   v)) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace, Brk>(c, ip, mem, memsize, gas);
  }
  c.m->regs_[ip->rd] = v;
  HOGD_STEP_NEXT(c, ip + 1);
}

template<bool Trace, uint32_t Width, bool Brk = false>
StepResult
op_store(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  if (!try_store<Width>(mem, memsize, c.m->regs_[ip->rs1], ip->rb.imm16,
                        c.m->regs_[ip->rb.rs2])) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace, Brk>(c, ip, mem, memsize, gas);
  }
  HOGD_STEP_NEXT(c, ip + 1);
}

// Fused handlers retain both architectural retirement steps when tracing.
// The tail slot remains executable as a branch target; normal fused execution
// skips it. Both instructions are covered by the run's charge.
#define HOGD_RETIRE_OP1(c, ip)                                                 \
  do {                                                                         \
    (c).m->gas_remaining_ = gas;                                               \
    (c).m->pc_ = pc_of((c), (ip) + 1);                                         \
    (c).m->obs_->on_retire(capture(c), StepResult::kContinue);                 \
  } while (0)

// Forward the loaded value directly to the consuming ALU operand. A load
// fault traps before the ALU executes; the load destination remains observable.
template<bool Trace, uint32_t Width, bool SignExtend, bool LoadedIsRs1,
         uint32_t (*Fn)(uint32_t, uint32_t)>
StepResult
op_load_rr(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  uint32_t v;
  if (!try_load<Width, SignExtend>(mem, memsize, c.m->regs_[ip->rs1], ip->imm,
                                   v)) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace>(c, ip, mem, memsize, gas);
  }
  c.m->regs_[ip->rd] = v;
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const Slot *rr = ip + 1;
  if constexpr (LoadedIsRs1) {
    c.m->regs_[rr->rd] = Fn(v, c.m->regs_[rr->rb.rs2]);
  }
  else {
    c.m->regs_[rr->rd] = Fn(c.m->regs_[rr->rs1], v);
  }
  HOGD_THREAD_NEXT(c, ip + 2);
}

// Load followed by an I-type consumer; rs1 receives the loaded value.
template<bool Trace, uint32_t Width, bool SignExtend,
         uint32_t (*Fn)(uint32_t, uint32_t)>
StepResult
op_load_ri(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  uint32_t v;
  if (!try_load<Width, SignExtend>(mem, memsize, c.m->regs_[ip->rs1], ip->imm,
                                   v)) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace>(c, ip, mem, memsize, gas);
  }
  c.m->regs_[ip->rd] = v;
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const Slot *ri = ip + 1;
  c.m->regs_[ri->rd] = Fn(v, bits_from_i32(ri->imm));
  HOGD_THREAD_NEXT(c, ip + 2);
}

// lui/addi materializes a constant. The head slot holds the combined value;
// the preserved tail immediate reconstructs the intermediate lui for tracing.
template<bool Trace>
StepResult
op_lui_addi(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  if constexpr (Trace) {
    // Retire the lui as its own step so the trace is byte-identical to the
    // unfused pair: lui writes the upper immediate (the full constant minus the
    // addi's, recovered from the preserved addi slot at ip+1).
    c.m->regs_[ip->rd] = bits_from_i32(ip->imm) - bits_from_i32((ip + 1)->imm);
    HOGD_RETIRE_OP1(c, ip);
  }
  c.m->regs_[ip->rd] = bits_from_i32(ip->imm); // full 32-bit constant
  HOGD_THREAD_NEXT(c, ip + 2);
}

// auipc/jalr has a static target checked during preparation. The head slot
// stores the auipc result in imm, its destination in rs1, and the link register
// in rd. The tail retains jalr's immediate and standalone branch-in handler.
template<bool Trace>
StepResult
op_auipc_jalr(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize,
              uint64_t gas)
{
  const uint32_t auipc_val = bits_from_i32(ip->imm); // head_pc + %hi, static
  // auipc dest == jalr base; visible to the callee, so write it.
  c.m->regs_[ip->rs1] = auipc_val;
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const uint32_t target =
      (auipc_val + bits_from_i32((ip + 1)->imm)) & ~uint32_t{1};
  c.m->regs_[ip->rd] =
      pc_of(c, ip) + (2 * kInstructionBytes); // link == pc_jalr + 4
  const Slot *next = c.base + (target / kInstructionBytes);
  HOGD_ENTER_RUN(c, next);
}

// --- Generic two-op fused handlers
// --------------------------------------------
//
// These read each op's operands straight from its own slot -- op1 from ip, op2
// from the preserved ip+1 -- so x0 remapping (baked into the slot indices) and
// op2's data dependence on op1's dest just work, with no repacking. They retire
// op1 as its own step in traced mode (capturing post-op1 state) so the observed
// trace is byte-identical to the unfused pair. Imm{1,2} pick a slot's I-type
// (immediate) vs R-type (rs2) second operand: an I-type slot carries the wide
// `imm`, an R-type the {imm16, rs2} arm. Memory ops trap at their own pc
// (op1 -> ip, op2 -> ip+1); op1's load/store fault check runs before op1
// retires, op2's after.

// alu ; alu  (e.g. sll;or, addi;addi, addi;sll, srl;addi)
template<bool Trace, uint32_t (*Fn1)(uint32_t, uint32_t), bool Imm1,
         uint32_t (*Fn2)(uint32_t, uint32_t), bool Imm2>
StepResult
op_alu_alu(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  const uint32_t b1 = Imm1 ? bits_from_i32(ip->imm) : c.m->regs_[ip->rb.rs2];
  c.m->regs_[ip->rd] = Fn1(c.m->regs_[ip->rs1], b1);
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const Slot *op2 = ip + 1;
  const uint32_t b2 = Imm2 ? bits_from_i32(op2->imm) : c.m->regs_[op2->rb.rs2];
  c.m->regs_[op2->rd] = Fn2(c.m->regs_[op2->rs1], b2);
  HOGD_THREAD_NEXT(c, ip + 2);
}

// alu ; store  (e.g. addi;sw -- compute an address/value, then store). The
// store can fault, terminal at its own pc (ip+1), after op1 has retired.
template<bool Trace, uint32_t (*Fn1)(uint32_t, uint32_t), bool Imm1,
         uint32_t Width>
StepResult
op_alu_store(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize,
             uint64_t gas)
{
  const uint32_t b1 = Imm1 ? bits_from_i32(ip->imm) : c.m->regs_[ip->rb.rs2];
  c.m->regs_[ip->rd] = Fn1(c.m->regs_[ip->rs1], b1);
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const Slot *st = ip + 1;
  if (!try_store<Width>(mem, memsize, c.m->regs_[st->rs1], st->rb.imm16,
                        c.m->regs_[st->rb.rs2])) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace>(c, st, mem, memsize, gas);
  }
  HOGD_THREAD_NEXT(c, ip + 2);
}

// alu ; load  (e.g. addi;lw -- compute an address, then load). The load can
// fault, terminal at its own pc (ip+1), after op1 has retired.
template<bool Trace, uint32_t (*Fn1)(uint32_t, uint32_t), bool Imm1,
         uint32_t Width, bool SignExtend>
StepResult
op_alu_load(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize, uint64_t gas)
{
  const uint32_t b1 = Imm1 ? bits_from_i32(ip->imm) : c.m->regs_[ip->rb.rs2];
  c.m->regs_[ip->rd] = Fn1(c.m->regs_[ip->rs1], b1);
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const Slot *ld = ip + 1;
  uint32_t v;
  if (!try_load<Width, SignExtend>(mem, memsize, c.m->regs_[ld->rs1], ld->imm,
                                   v)) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace>(c, ld, mem, memsize, gas);
  }
  c.m->regs_[ld->rd] = v;
  HOGD_THREAD_NEXT(c, ip + 2);
}

// store ; alu  (e.g. sw;addi -- store, then advance the pointer). The store
// (op1) faults at its own pc (ip) before retiring; op2 is a plain ALU.
template<bool Trace, uint32_t Width, uint32_t (*Fn2)(uint32_t, uint32_t),
         bool Imm2>
StepResult
op_store_alu(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize,
             uint64_t gas)
{
  if (!try_store<Width>(mem, memsize, c.m->regs_[ip->rs1], ip->rb.imm16,
                        c.m->regs_[ip->rb.rs2])) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace>(c, ip, mem, memsize, gas);
  }
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const Slot *op2 = ip + 1;
  const uint32_t b2 = Imm2 ? bits_from_i32(op2->imm) : c.m->regs_[op2->rb.rs2];
  c.m->regs_[op2->rd] = Fn2(c.m->regs_[op2->rs1], b2);
  HOGD_THREAD_NEXT(c, ip + 2);
}

// load ; store  (e.g. lw;sw -- word copy / memcpy body). Either can fault,
// terminal at its own pc: the load at ip (before it retires), the store at
// ip+1 (after).
template<bool Trace, uint32_t LWidth, bool LSignExtend, uint32_t SWidth>
StepResult
op_load_store(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize,
              uint64_t gas)
{
  uint32_t v;
  if (!try_load<LWidth, LSignExtend>(mem, memsize, c.m->regs_[ip->rs1], ip->imm,
                                     v)) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace>(c, ip, mem, memsize, gas);
  }
  c.m->regs_[ip->rd] = v;
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const Slot *st = ip + 1;
  if (!try_store<SWidth>(mem, memsize, c.m->regs_[st->rs1], st->rb.imm16,
                         c.m->regs_[st->rb.rs2])) {
    HOGD_MUSTTAIL return trap_bad_memory<Trace>(c, st, mem, memsize, gas);
  }
  HOGD_THREAD_NEXT(c, ip + 2);
}

// alu ; conditional branch  (e.g. addi;bltu -- loop induction + bound check).
// op2 is a barrier: it ends the run, entering the taken target or the
// fall-through. create() only fuses when the branch target is in range, so
// br->rb.imm16 is the resolved slot delta (as for op_branch).
template<bool Trace, uint32_t (*Fn1)(uint32_t, uint32_t), bool Imm1,
         bool (*Cmp)(uint32_t, uint32_t)>
StepResult
op_alu_branch(Ctx c, const Slot *ip, uint8_t *mem, uint32_t memsize,
              uint64_t gas)
{
  const uint32_t b1 = Imm1 ? bits_from_i32(ip->imm) : c.m->regs_[ip->rb.rs2];
  c.m->regs_[ip->rd] = Fn1(c.m->regs_[ip->rs1], b1);
  if constexpr (Trace) {
    HOGD_RETIRE_OP1(c, ip);
  }
  const Slot *br = ip + 1;
  if (!Cmp(c.m->regs_[br->rs1], c.m->regs_[br->rb.rs2])) {
    HOGD_ENTER_RUN(c, ip + 2); // not taken: past the branch
  }
  const Slot *next = br + br->rb.imm16; // taken: resolved target delta from br
  HOGD_ENTER_RUN(c, next);
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
  // A deficit (speculative) run must settle before the host boundary: a
  // syscall has effects outside the frame that canonical -- which stopped at
  // the run's first unpayable instruction -- never performs. Every other run
  // exit funnels through enter_run or trap_at, which carry the same check.
  if (c.m->gas_deficit_ != 0) [[unlikely]] {
    HOGD_MUSTTAIL return trap_at<Trace, Trap::kOutOfGas>(c, ip, mem, memsize,
                                                         /*gas=*/0);
  }
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

template<bool Trace, bool Brk = false>
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
    return &op_lui<Trace, Brk>;
  case Op::kAuipc:
    return &op_auipc<Trace, Brk>;
  case Op::kAddi:
    return &op_ri<Trace, add_op, Brk>;
  case Op::kSlti:
    return &op_ri<Trace, slt_op, Brk>;
  case Op::kSltiu:
    return &op_ri<Trace, sltu_op, Brk>;
  case Op::kXori:
    return &op_ri<Trace, xor_op, Brk>;
  case Op::kOri:
    return &op_ri<Trace, or_op, Brk>;
  case Op::kAndi:
    return &op_ri<Trace, and_op, Brk>;
  case Op::kSlli:
    return &op_ri<Trace, sll_op, Brk>;
  case Op::kSrli:
    return &op_ri<Trace, srl_op, Brk>;
  case Op::kSrai:
    return &op_ri<Trace, sra_op, Brk>;
  case Op::kAdd:
    return &op_rr<Trace, add_op, Brk>;
  case Op::kSub:
    return &op_rr<Trace, sub_op, Brk>;
  case Op::kSll:
    return &op_rr<Trace, sll_op, Brk>;
  case Op::kSlt:
    return &op_rr<Trace, slt_op, Brk>;
  case Op::kSltu:
    return &op_rr<Trace, sltu_op, Brk>;
  case Op::kXor:
    return &op_rr<Trace, xor_op, Brk>;
  case Op::kSrl:
    return &op_rr<Trace, srl_op, Brk>;
  case Op::kSra:
    return &op_rr<Trace, sra_op, Brk>;
  case Op::kOr:
    return &op_rr<Trace, or_op, Brk>;
  case Op::kAnd:
    return &op_rr<Trace, and_op, Brk>;
  case Op::kMul:
    return &op_rr<Trace, mul_low, Brk>;
  case Op::kMulh:
    return &op_rr<Trace, mulh, Brk>;
  case Op::kMulhsu:
    return &op_rr<Trace, mulhsu, Brk>;
  case Op::kMulhu:
    return &op_rr<Trace, mulhu, Brk>;
  case Op::kDiv:
    return &op_rr<Trace, div_signed, Brk>;
  case Op::kDivu:
    return &op_rr<Trace, div_unsigned, Brk>;
  case Op::kRem:
    return &op_rr<Trace, rem_signed, Brk>;
  case Op::kRemu:
    return &op_rr<Trace, rem_unsigned, Brk>;
  case Op::kLb:
    return &op_load<Trace, 1, true, Brk>;
  case Op::kLh:
    return &op_load<Trace, 2, true, Brk>;
  case Op::kLw:
    return &op_load<Trace, 4, false, Brk>;
  case Op::kLbu:
    return &op_load<Trace, 1, false, Brk>;
  case Op::kLhu:
    return &op_load<Trace, 2, false, Brk>;
  case Op::kSb:
    return &op_store<Trace, 1, Brk>;
  case Op::kSh:
    return &op_store<Trace, 2, Brk>;
  case Op::kSw:
    return &op_store<Trace, 4, Brk>;
  case Op::kFence:
    return &op_fence<Trace, Brk>;
  case Op::kEbreak:
    // ebreak's gas is charged at the run head; the handler is a pure trap, so
    // dispatch straight to trap_at (it has the handler signature).
    return &trap_at<Trace, Trap::kBreakpoint>;
  case Op::kUnimp:
    // unimp's gas is charged at the run head; the handler is a pure trap, so
    // dispatch straight to trap_at (it has the handler signature).
    return &trap_at<Trace, Trap::kInvalidInstruction>;
  case Op::kEcall:
    return &op_ecall<Trace>;
  default:
    // instruction_at() rejects unsupported opcodes before handler selection.
    return &trap_at<Trace, Trap::kInvalidInstruction>;
  }
}

// Select a handler from the runtime (trace, brk) pair. brk is set only for a
// mid-run break carrier (a non-barrier slot ending a chunk).
inline Handler
pick_handler(Op op, bool trace, bool brk)
{
  if (brk) {
    return trace ? handler_for<true, true>(op) : handler_for<false, true>(op);
  }
  return trace ? handler_for<true, false>(op) : handler_for<false, false>(op);
}

// Called during preparation for conditional branches only.
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

// Fuse lw followed by a supported ALU consumer. Restricting the opcode set
// bounds handler instantiations. loaded_is_rs1 selects the forwarded R-type
// operand; I-type consumers always use rs1.
template<bool Trace>
Handler
fused_load_handler(Op load_op, Op next_op, bool loaded_is_rs1)
{
  if (load_op != Op::kLw) {
    return nullptr;
  }
  switch (next_op) {
  // R-type tail: feed the loaded value into rs1 or rs2 per loaded_is_rs1.
  case Op::kAdd:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, add_op>
                         : &op_load_rr<Trace, 4, false, false, add_op>;
  case Op::kSub:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, sub_op>
                         : &op_load_rr<Trace, 4, false, false, sub_op>;
  case Op::kSll:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, sll_op>
                         : &op_load_rr<Trace, 4, false, false, sll_op>;
  case Op::kSlt:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, slt_op>
                         : &op_load_rr<Trace, 4, false, false, slt_op>;
  case Op::kSltu:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, sltu_op>
                         : &op_load_rr<Trace, 4, false, false, sltu_op>;
  case Op::kXor:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, xor_op>
                         : &op_load_rr<Trace, 4, false, false, xor_op>;
  case Op::kSrl:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, srl_op>
                         : &op_load_rr<Trace, 4, false, false, srl_op>;
  case Op::kSra:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, sra_op>
                         : &op_load_rr<Trace, 4, false, false, sra_op>;
  case Op::kOr:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, or_op>
                         : &op_load_rr<Trace, 4, false, false, or_op>;
  case Op::kAnd:
    return loaded_is_rs1 ? &op_load_rr<Trace, 4, false, true, and_op>
                         : &op_load_rr<Trace, 4, false, false, and_op>;
  // I-type tail (second operand is the immediate; loaded value is always rs1).
  case Op::kAddi:
    return &op_load_ri<Trace, 4, false, add_op>;
  case Op::kSlti:
    return &op_load_ri<Trace, 4, false, slt_op>;
  case Op::kSltiu:
    return &op_load_ri<Trace, 4, false, sltu_op>;
  case Op::kXori:
    return &op_load_ri<Trace, 4, false, xor_op>;
  case Op::kOri:
    return &op_load_ri<Trace, 4, false, or_op>;
  case Op::kAndi:
    return &op_load_ri<Trace, 4, false, and_op>;
  case Op::kSlli:
    return &op_load_ri<Trace, 4, false, sll_op>;
  case Op::kSrli:
    return &op_load_ri<Trace, 4, false, srl_op>;
  case Op::kSrai:
    return &op_load_ri<Trace, 4, false, sra_op>;
  default:
    return nullptr;
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

// A fusion rule replaces the head handler and may repack its operands. The
// tail remains intact for branches into the second instruction.
struct FusionCtx {
  const Instr &head;              // slot i
  const Instr &tail;              // slot i+1
  uint32_t head_pc;               // head's byte pc (auipc+jalr target math)
  const ThreadedProgram &program; // PC-relative target range checks
  bool trace;
};
using FusionRule = bool (*)(const FusionCtx &, Slot &);

// lui rd, %hi ; addi rd, rd, %lo  ->  li rd, imm32.
inline bool
fuse_lui_addi(const FusionCtx &f, Slot &s)
{
  if (f.head.op != Op::kLui || f.tail.op != Op::kAddi || f.head.rd == 0 ||
      f.tail.rd != f.head.rd || f.tail.rs1 != f.head.rd) {
    return false;
  }
  s.handler = f.trace ? &op_lui_addi<true> : &op_lui_addi<false>;
  s.imm = static_cast<int32_t>(bits_from_i32(f.head.imm) +
                               bits_from_i32(f.tail.imm));
  return true;
}

// auipc t, %hi ; jalr ra, %lo(t)  ->  static far call. Only when the (static)
// target is in range, so op_auipc_jalr needs no per-execution range check.
inline bool
fuse_auipc_jalr(const FusionCtx &f, Slot &s)
{
  if (f.head.op != Op::kAuipc || f.tail.op != Op::kJalr || f.head.rd == 0 ||
      f.tail.rs1 != f.head.rd) {
    return false;
  }
  const uint32_t auipc_val = f.head_pc + bits_from_i32(f.head.imm);
  const uint32_t target =
      (auipc_val + bits_from_i32(f.tail.imm)) & ~uint32_t{1};
  if (!f.program.contains_pc(target)) {
    return false;
  }
  s.handler = f.trace ? &op_auipc_jalr<true> : &op_auipc_jalr<false>;
  s.rd = static_cast<uint8_t>(f.tail.rd);  // link reg (x0 -> trash)
  s.rs1 = static_cast<uint8_t>(f.head.rd); // auipc dest == jalr base (!= 0)
  // The finished (static) auipc result; the handler reads the jalr's 12-bit
  // imm from the preserved tail slot, so nothing else needs packing.
  s.imm = static_cast<int32_t>(auipc_val);
  return true;
}

// lw rd, off(rs1) ; <fusible ALU consuming rd>  ->  load+use.
inline bool
fuse_load_use(const FusionCtx &f, Slot &s)
{
  const Instr &ld = f.head;
  const Instr &alu = f.tail;
  if (ld.rd == 0) {
    return false;
  }
  const bool rs1_match = alu.rs1 == ld.rd;
  const bool rs2_match = slot_packs_rs2(alu.op) && alu.rs2 == ld.rd;
  if (!rs1_match && !rs2_match) {
    return false;
  }
  const Handler fused =
      f.trace ? fused_load_handler<true>(ld.op, alu.op, rs1_match)
              : fused_load_handler<false>(ld.op, alu.op, rs1_match);
  if (fused == nullptr) {
    return false;
  }
  s.handler = fused;
  return true;
}

// Supported adjacent ALU pairs. The second operation reads the registers
// written by the first; no register-alias restriction is needed.
template<bool Trace>
Handler
alu_alu_handler(Op op1, Op op2)
{
  if (op1 == Op::kSll && op2 == Op::kOr) { // shift-or combine
    return &op_alu_alu<Trace, sll_op, false, or_op, false>;
  }
  if (op1 == Op::kAddi && op2 == Op::kAddi) { // back-to-back address math
    return &op_alu_alu<Trace, add_op, true, add_op, true>;
  }
  if (op1 == Op::kAddi && op2 == Op::kSll) { // offset then scale
    return &op_alu_alu<Trace, add_op, true, sll_op, false>;
  }
  if (op1 == Op::kSrl && op2 == Op::kAddi) { // shift then offset
    return &op_alu_alu<Trace, srl_op, false, add_op, true>;
  }
  return nullptr;
}

inline bool
fuse_alu_alu(const FusionCtx &f, Slot &s)
{
  const Handler h = f.trace ? alu_alu_handler<true>(f.head.op, f.tail.op)
                            : alu_alu_handler<false>(f.head.op, f.tail.op);
  if (h == nullptr) {
    return false;
  }
  s.handler = h;
  return true;
}

// addi ; store : compute an address/value, then store it.
template<bool Trace>
Handler
alu_store_handler(Op alu, Op store)
{
  if (alu != Op::kAddi) {
    return nullptr;
  }
  switch (store) {
  case Op::kSb:
    return &op_alu_store<Trace, add_op, true, 1>;
  case Op::kSh:
    return &op_alu_store<Trace, add_op, true, 2>;
  case Op::kSw:
    return &op_alu_store<Trace, add_op, true, 4>;
  default:
    return nullptr;
  }
}

inline bool
fuse_alu_store(const FusionCtx &f, Slot &s)
{
  const Handler h = f.trace ? alu_store_handler<true>(f.head.op, f.tail.op)
                            : alu_store_handler<false>(f.head.op, f.tail.op);
  if (h == nullptr) {
    return false;
  }
  s.handler = h;
  return true;
}

// addi ; load : compute an address, then load from it.
template<bool Trace>
Handler
alu_load_handler(Op alu, Op load)
{
  if (alu != Op::kAddi) {
    return nullptr;
  }
  switch (load) {
  case Op::kLb:
    return &op_alu_load<Trace, add_op, true, 1, true>;
  case Op::kLh:
    return &op_alu_load<Trace, add_op, true, 2, true>;
  case Op::kLw:
    return &op_alu_load<Trace, add_op, true, 4, false>;
  case Op::kLbu:
    return &op_alu_load<Trace, add_op, true, 1, false>;
  case Op::kLhu:
    return &op_alu_load<Trace, add_op, true, 2, false>;
  default:
    return nullptr;
  }
}

inline bool
fuse_alu_load(const FusionCtx &f, Slot &s)
{
  const Handler h = f.trace ? alu_load_handler<true>(f.head.op, f.tail.op)
                            : alu_load_handler<false>(f.head.op, f.tail.op);
  if (h == nullptr) {
    return false;
  }
  s.handler = h;
  return true;
}

// store ; addi : store, then advance the pointer.
template<bool Trace>
Handler
store_alu_handler(Op store, Op alu)
{
  if (alu != Op::kAddi) {
    return nullptr;
  }
  switch (store) {
  case Op::kSb:
    return &op_store_alu<Trace, 1, add_op, true>;
  case Op::kSh:
    return &op_store_alu<Trace, 2, add_op, true>;
  case Op::kSw:
    return &op_store_alu<Trace, 4, add_op, true>;
  default:
    return nullptr;
  }
}

inline bool
fuse_store_alu(const FusionCtx &f, Slot &s)
{
  const Handler h = f.trace ? store_alu_handler<true>(f.head.op, f.tail.op)
                            : store_alu_handler<false>(f.head.op, f.tail.op);
  if (h == nullptr) {
    return false;
  }
  s.handler = h;
  return true;
}

// lw ; sw : word copy (memcpy body).
inline bool
fuse_load_store(const FusionCtx &f, Slot &s)
{
  if (f.head.op != Op::kLw || f.tail.op != Op::kSw) {
    return false;
  }
  s.handler = f.trace ? &op_load_store<true, 4, false, 4>
                      : &op_load_store<false, 4, false, 4>;
  return true;
}

// addi ; <cond branch> : loop induction + bound check. Only when the branch
// target is in range (so the tail slot's imm16 is the resolved delta).
template<bool Trace>
Handler
alu_branch_handler(Op alu, Op branch)
{
  if (alu != Op::kAddi) {
    return nullptr;
  }
  switch (branch) {
  case Op::kBeq:
    return &op_alu_branch<Trace, add_op, true, beq_cmp>;
  case Op::kBne:
    return &op_alu_branch<Trace, add_op, true, bne_cmp>;
  case Op::kBlt:
    return &op_alu_branch<Trace, add_op, true, blt_cmp>;
  case Op::kBge:
    return &op_alu_branch<Trace, add_op, true, bge_cmp>;
  case Op::kBltu:
    return &op_alu_branch<Trace, add_op, true, bltu_cmp>;
  case Op::kBgeu:
    return &op_alu_branch<Trace, add_op, true, bgeu_cmp>;
  default:
    return nullptr;
  }
}

inline bool
fuse_alu_branch(const FusionCtx &f, Slot &s)
{
  if (f.head.op != Op::kAddi || !is_conditional_branch(f.tail.op)) {
    return false;
  }
  // The branch sits at head_pc + 4; only fuse an in-range target (its slot's
  // imm16 is the resolved delta, which op_alu_branch consumes).
  const uint32_t branch_pc = f.head_pc + kInstructionBytes;
  const uint32_t target = branch_pc + bits_from_i32(f.tail.imm);
  if (!f.program.contains_pc(target)) {
    return false;
  }
  const Handler h = f.trace ? alu_branch_handler<true>(f.head.op, f.tail.op)
                            : alu_branch_handler<false>(f.head.op, f.tail.op);
  if (h == nullptr) {
    return false;
  }
  s.handler = h;
  return true;
}

// Rules are tried in order, first match wins. The head ops are disjoint per
// rule (lui / auipc / lw / ...), or the rules differ on the tail, so the order
// needs no priority tuning.
inline constexpr std::array<FusionRule, 9> kFusionRules = {
    &fuse_lui_addi,  &fuse_auipc_jalr, &fuse_load_use,
    &fuse_alu_alu,   &fuse_alu_store,  &fuse_alu_load,
    &fuse_store_alu, &fuse_load_store, &fuse_alu_branch};

inline std::expected<ThreadedMachinePreInstantiation, ProgramError>
ThreadedMachinePreInstantiation::create(std::span<const uint8_t> code,
                                        std::span<const uint8_t> memory,
                                        uint32_t memory_start,
                                        uint32_t total_memory_size, bool trace,
                                        uint32_t max_run_gas, bool fuse)
{
  max_run_gas = std::clamp<uint32_t>(max_run_gas,
                                     static_cast<uint32_t>(kMaxInstructionGas),
                                     kMaxRunGas);
  const auto program = ThreadedProgram::create(code);
  if (!program.has_value()) {
    return std::unexpected(program.error());
  }

  // Check the fit with subtraction so the guard is independent of host pointer
  // width: memory_start + memory.size() can wrap when size_t is 32 bits, which
  // would let an out-of-bounds image slip through to the copy below.
  if (total_memory_size > kMemoryCeilingBytes ||
      memory_start > total_memory_size ||
      memory.size() > total_memory_size - memory_start) {
    return std::unexpected(ProgramError::kMemoryTooLarge);
  }

  std::vector<uint8_t> initial_memory_image(total_memory_size, 0);
  std::ranges::copy(memory, initial_memory_image.begin() + memory_start);

  std::vector<Slot> exec;
  size_t n = program->instruction_count();
  exec.resize(n + 1);

  // Sentinel for sequential fall-off past the last instruction: trap kBadPc at
  // the out-of-range pc (its pc is (ip - base) * 4). No gas is charged.
  exec[n] = Slot{.handler = trace ? &trap_at<true, Trap::kBadPc>
                                  : &trap_at<false, Trap::kBadPc>};

  // Decode, price, and fuse in one backward pass. Carry the successor's
  // decoded instruction, whether it heads a fused pair, and whether it ends
  // a chunk. These guards prevent overlapping pairs and skipped chunk charges.
  Instr next_op = {};
  bool next_fused = false;
  bool next_is_break = false;

  for (size_t i = n; i-- > 0;) {
    const auto instruction = program->instruction_at(i);
    if (!instruction.has_value()) {
      return std::unexpected(instruction.error());
    }
    const Instr &d = *instruction;
    Slot &slot = exec[i];

    // Accumulate suffix gas backward. If adding this instruction would exceed
    // max_run_gas, make it a break carrier: its handler enters a new chunk at
    // i+1. The successor's sum is already available, so no break array is needed.
    const auto self_gas = static_cast<uint32_t>(d.gas);
    assert(self_gas <= max_run_gas &&
           "a single instruction's gas must fit within one run chunk");
    bool is_break_here = false;
    uint32_t run_gas = self_gas;
    if (!is_barrier(d.op) && i + 1 < n) {
      const uint32_t candidate = self_gas + exec[i + 1].gas_to_run;
      if (candidate <= max_run_gas) {
        run_gas = candidate;
      }
      else {
        is_break_here = true; // break carrier; run_gas stays self_gas
      }
    }
    assert(run_gas <= max_run_gas);
    slot.gas_to_run = static_cast<uint16_t>(run_gas);

    slot.handler = pick_handler(d.op, trace, is_break_here);

    // x0 reads come from the always zero register at regs_[kRegisterCount].
    // writes go to trash at regs_[0].  This keeps the write-heavy actions
    // within 2 cache lines, instead of 3.
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
    // the wide imm arm (its slot delta can exceed int16); a branch's fits
    // int16. Out of range: swap in the op-specific trapping handler.
    if (d.op == Op::kJal || is_conditional_branch(d.op)) {
      const uint32_t pc = static_cast<uint32_t>(i) * kInstructionBytes;
      const uint32_t target = pc + bits_from_i32(d.imm);
      if (program->contains_pc(target)) {
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

    // Fuse disjoint adjacent pairs. The tail retains its standalone handler
    // and suffix gas so branches can still enter it. Calibration disables
    // fusion when measuring individual opcode costs.
    bool fused_here = false;
    // A fused handler skips the intervening dispatch, so neither member may
    // be a break carrier: that would skip charging the next chunk.
    if (fuse && i + 1 < n && !next_fused && !is_break_here && !next_is_break) {
      const FusionCtx fctx{.head = d,
                           .tail = next_op,
                           .head_pc =
                               static_cast<uint32_t>(i) * kInstructionBytes,
                           .program = *program,
                           .trace = trace};
      for (const FusionRule rule : kFusionRules) {
        if (rule(fctx, exec[i])) {
          fused_here = true;
          break;
        }
      }
    }
    next_op = d;
    next_fused = fused_here;
    next_is_break = is_break_here;
  }

  return ThreadedMachinePreInstantiation(std::move(exec),
                                         std::move(initial_memory_image),
                                         trace);
}

inline MachineView
ThreadedMachine::view()
{
  regs_[0] = 0; // regs_[0] is the x0 trash sink; present architectural 0
  return MachineView(pc_, gas_remaining_,
                     std::span<const uint32_t, kRegisterCount>(regs_.data(),
                                                               kRegisterCount),
                     memory_.bytes());
}

template<bool Trace>
StepResult
ThreadedMachine::drive()
{
  Ctx c{.m = this, .base = exec_.data()};
  const Slot *ip = c.base + (pc_ / kInstructionBytes);
  uint8_t *mem = memory_.bytes().data();
  const auto memsize = static_cast<uint32_t>(memory_.bytes().size());
  // Plain call+return, not a musttail: a non-static member function cannot
  // tail-call the free function enter_run (musttail needs matching signatures,
  // and the implicit `this` differs). enter_run begins the musttail chain,
  // which runs in O(1) stack; this is the single non-recursive bridge frame
  // into it. Seed the register-carried gas from the struct; the dispatch chain
  // carries it onward and spills it back at terminal/observe/ecall points.
  return enter_run<Trace>(c, ip, mem, memsize, gas_remaining_);
}

// Apply the host result and retire the ecall; drive() resumes execution.
template<bool Trace>
StepResult
ThreadedMachine::apply_ecall(const HostResult &result)
{
  assert(pending_ecall_.has_value());
  pending_ecall_.reset();

  // The production driver handles host errors before resuming. This engine
  // has no HostTrapPayload, so accepting kTrap would lose its classification.
  // Check before debiting gas so an excessive charge cannot mask API misuse.
  if (result.status == HostStatus::kTrap) {
    assert(false && "ThreadedMachine::apply_ecall: HostStatus::kTrap is not "
                    "supported on the threaded ecall path");
    std::abort();
  }

  // pc_ is already pc_after (set in op_ecall); rebuild Ctx/next for trap_at and
  // capture(). next's pc == pc_after, matching the inline trap_at(..., next).
  Ctx c{.m = this, .base = exec_.data()};
  const Slot *next = c.base + (pc_ / kInstructionBytes);
  uint8_t *mem = memory_.bytes().data();
  const auto memsize = static_cast<uint32_t>(memory_.bytes().size());

  // Debit gas before applying the register result. Host memory writes have
  // already occurred and persist on OOG; there is no per-ecall rollback.
  if (gas_remaining_ < result.gas_consumed) {
    gas_remaining_ = 0;
    return trap_at<Trace, Trap::kOutOfGas>(c, next, mem, memsize,
                                           gas_remaining_);
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
    gas_remaining_ = 0;
    return trap_at<Trace, Trap::kOutOfGas>(c, next, mem, memsize,
                                           gas_remaining_);
  case HostStatus::kTrap:
  default:
    // kTrap is forbidden above; any status reaching here is a contract
    // violation (e.g. an unknown enumerator), never a silent payload drop.
    assert(false && "ThreadedMachine::apply_ecall: unsupported HostStatus");
    std::abort();
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
  assert(!exec_traced_);
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
  assert(exec_traced_);
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
