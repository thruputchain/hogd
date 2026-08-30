#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/program.h"

// Deterministic kernel synthesis for gas calibration.
//
// A kernel is a loop whose body is a fixed number of *slots*. Every slot holds
// either the filler instruction or the target instruction under measurement;
// the sweep varies only how many slots are targets. Holding the slot count,
// code size, loop trip count, and register/operand pattern fixed across a
// sweep is what makes the resulting cost estimate a clean contrast instead of
// a regression coefficient: the only thing that differs between two kernels in
// a sweep is the identity of `target_slots` opcodes, so the fixed per-iteration
// overhead (loop induction, back edge, run entry, timer) lands entirely in the
// fit's intercept and cancels out of the slope.
//
// The filler is `add rd, rs1, rs2` on registers disjoint from every target
// operand. R-type ADD is the one ALU op that no fusion rule in
// threaded_machine.h will fuse in either position: as a head it matches
// neither `alu_alu_handler` (which wants sll/addi/srl) nor any rule keyed on
// lui/auipc/load/store/addi, and as a tail it is not or/addi/sll/load/store/
// branch. Register disjointness additionally keeps `fuse_load_use` from
// pairing a load target with the filler that follows it. lowering_audit.h
// verifies that property per kernel rather than trusting this reasoning.
namespace hogd::rv32::gas_calibration {

// Register assignment. Disjoint banks keep the filler, the loop control, and
// every target operand from aliasing, which is what keeps fusion off and keeps
// the dependence structure of a sweep constant.
//
// The filler writes a *rotating* destination and reads only registers the body
// never writes, so consecutive fillers have no true dependence on each other.
// That matters more than it looks: a filler that chains through one register
// (add x14, x14, x15) makes the all-filler baseline store-forward-latency
// bound rather than dispatch-throughput bound, and then substituting any
// target that writes elsewhere *shortens* the chain and measures as a negative
// cost. The rotating filler removes the chain, so the baseline measures what
// it claims to -- the engine's cost of retiring one more instruction.
inline constexpr uint8_t kSrcLargeReg = 1; // never written by the body
inline constexpr uint8_t kLoopCounterReg = 2;
inline constexpr uint8_t kSrcSmallReg = 3;   // never written by the body
inline constexpr uint8_t kStoreValueReg = 4; // never written by the body
// Rotating destinations for targets in the independent-operand context.
inline constexpr uint8_t kDataBankBase = 5;
inline constexpr uint8_t kDataBankSize = 8; // x5..x12
// Single register for the serial-dependence context.
inline constexpr uint8_t kChainReg = 13;
// Rotating filler destinations.
inline constexpr uint8_t kFillerDstBase = 14;
inline constexpr uint8_t kFillerDstSize = 4; // x14..x17
// Holds the address a JALR's immediate is measured from; see target_word.
inline constexpr uint8_t kJalrBaseReg = 31;
// Address bases for memory targets, spread over the working set.
inline constexpr uint8_t kAddressBankBase = 18;
inline constexpr uint8_t kAddressBankSize = 8; // x18..x25
// Destinations for load targets. Disjoint from the filler's source registers,
// so the filler after a load never consumes the loaded value and fuse_load_use
// cannot pair them.
inline constexpr uint8_t kLoadDstBase = 26;
inline constexpr uint8_t kLoadDstSize = 4; // x26..x29
// Holds 1. Chaining a divide through `rd = rd / 1` is the only way to build a
// true divide latency chain: any other divisor shrinks the dividend toward
// zero within a few slots, after which the sweep is measuring the divider's
// zero fast path instead of the case it set out to measure.
inline constexpr uint8_t kOneReg = 30;

// Operand shape for the target instruction. This is the axis that turns "the
// cost of MUL" into a measurable range rather than a single number: the same
// opcode costs differently when its results feed the next instruction than
// when they do not, and the price an adversary can force is the worse one.
enum class OperandContext : uint8_t {
  // Every target writes and reads one register: a serial dependence chain, so
  // the target's own latency is exposed.
  kChain,
  // Targets rotate over an 8-register bank: no target-to-target dependence,
  // so the measurement is throughput-bound.
  kIndependent,
};

// Where a memory target's addresses land. Guest memory is capped at
// kMaxMemoryBytes (64 KiB), which is smaller than this machine class's L2, so
// the spread between these two is the whole cache-behaviour range a guest can
// reach -- there is no cold-DRAM case to price.
enum class WorkingSet : uint8_t {
  kNone,      // non-memory target
  kL1Region,  // all bases inside one 4 KiB region
  kFullRange, // bases spread across the full guest memory
  // Dependent loads: `lw rd, 0(rd)` walking a shuffled cycle through the full
  // 64 KiB. Each load's address is the previous load's result, so the engine
  // cannot overlap them and every one pays full cache latency. Independent
  // loads measure load throughput, which the interpreter's own dispatch hides;
  // this measures load latency, which it cannot. A guest that wants the most
  // work per gas out of a load writes exactly this, so this is the arrangement
  // the schedule has to be able to afford. LW only -- a narrower load cannot
  // carry a pointer.
  kPointerChase,
  // Every store to one address. A store produces no value, so it cannot chain
  // through its result the way a load can -- but repeatedly hitting one address
  // contends for the same store-buffer entry, which is the arrangement that
  // costs a store the most and which an independent-address sweep cannot see.
  kSameAddress,
};

// Operand magnitude class for the variable-latency M-extension ops. x86 integer
// divide latency depends on operand magnitude, so DIV/REM must be priced from
// the slow class, not an average.
enum class OperandMagnitude : uint8_t {
  kDefault,
  kSmall, // small dividend, small divisor
  kLarge, // full-width dividend, small divisor: longest divider latency
};

// True if `op` is an M-extension divide or remainder. These are the only
// opcodes whose cost depends on operand magnitude, and the only ones that need
// the divide-specific chain form.
[[nodiscard]] bool is_divide_target(Op op);

// An adjacent instruction *pair* to substitute, instead of a single opcode.
//
// Per-opcode sweeps can only measure instructions in isolation, so they cannot
// see a cost that exists only in the interaction between two of them. The
// validation workloads found one -- word_copy runs far slower than its opcodes
// predict, because each store writes the address the next load reads and the
// pair serializes on store-to-load forwarding.
//
// A pair substitution measures that directly: two adjacent slots replace two
// fillers, and the sweep's slope is nanoseconds per pair. The two kinds below
// differ only in whether the load reads the address the store just wrote, so
// the difference between them *is* the forwarding penalty, with the register
// chain and instruction mix held identical.
enum class PairKind : uint8_t {
  kNone,
  // sw rX, off(base) ; lw rX, off(base) -- the load reads back what the store
  // just wrote, so it waits on store-to-load forwarding.
  kStoreLoadForward,
  // sw rX, off(base) ; lw rX, off'(base) -- stores and loads use disjoint
  // regions, so nothing forwards. The control.
  kStoreLoadDistinct,
  // auipc rX, 0 ; jalr rY, rX, 8 -- the pc-relative call idiom, and the pair
  // fuse_auipc_jalr exists for. The jalr lands on the instruction after itself,
  // so the retired sequence is unchanged.
  kAuipcJalr,
};

struct KernelSpec {
  Op target = Op::kAdd;
  // When set, `target` is ignored and each substitution places this pair into
  // two adjacent slots instead.
  PairKind pair = PairKind::kNone;
  OperandContext context = OperandContext::kIndependent;
  WorkingSet working_set = WorkingSet::kNone;
  OperandMagnitude magnitude = OperandMagnitude::kDefault;
  // For a conditional-branch target: emit operands that take the branch. The
  // taken form jumps to the instruction it would otherwise fall through to, so
  // the iteration retires exactly the same instructions either way and the
  // difference measured is the taken path itself.
  bool branch_taken = false;
  // Body slot count, constant across a sweep.
  uint32_t body_slots = 256;
  // How many of those slots hold the target. Swept 0..body_slots/2 so that
  // spread placement never puts two targets adjacent.
  uint32_t target_slots = 0;
  // Rotates the target placement within the body without changing how many
  // targets there are. Two kernels differing only in phase do identical work,
  // so any difference between them is pure layout systematics -- code
  // alignment, which slots share a cache line, what the indirect-branch
  // predictor has learned. That bias is fixed per kernel, so repeating a
  // measurement does not remove it; measuring several phases at the same
  // target count does.
  uint32_t placement_phase = 0;
  // Loop iterations. Chosen by the driver so each kernel runs long enough to
  // dominate timer overhead.
  uint32_t trip_count = 64;
};

struct Kernel {
  std::vector<uint8_t> code;
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = kMaxMemoryBytes;
  // Instructions retired per loop iteration and over the whole run. Used to
  // cross-check the measurement against what actually executed.
  uint64_t instructions_per_iteration = 0;
  uint64_t dynamic_instructions = 0;
  // Gas the current schedule charges for the whole run.
  uint64_t scheduled_gas = 0;
  // Byte offsets of the body slots that hold the target instruction, in
  // program order. lowering_audit.h uses these to check the lowering.
  std::vector<uint32_t> target_slot_pcs;
  // Byte offset of the first body slot.
  uint32_t body_start_pc = 0;
};

// Why a kernel could not be built. Reported rather than collapsed into an empty
// optional so a rejected sweep says which constraint it hit -- and so the
// harness matches the rest of runtime/rv32, where every fallible construction
// (Program::create, ThreadedProgram::create, PreInstantiation::create) returns
// std::expected with a named error.
enum class KernelError {
  kUnsupportedTarget, // no body-slot encoding for this opcode
  kBodyTooSmall,      // fewer slots than the fixed boundaries need
  kZeroTripCount,
  kTooManyTargets, // would put two targets adjacent, exposing them to fusion
  kCodeTooLarge,   // over kMaxCodeBytes
  kUndecodable,    // generated code did not round-trip through the decoder
};

[[nodiscard]] std::string kernel_error_name(KernelError error);

// True for opcodes this harness knows how to place in a body slot. Excluded:
// control transfers whose target would have to be rewritten per slot (JAL,
// JALR), the trapping/halting ops (ECALL, EBREAK, UNIMP), and FENCE, which the
// substitution design cannot hold constant. Branches are included: a
// not-taken conditional branch is a legal body slot and is exactly how the
// harness prices a run barrier.
[[nodiscard]] bool is_measurable_target(Op op);

// True if the op reads or writes guest memory.
[[nodiscard]] bool is_memory_target(Op op);

// True if the op reads memory.
[[nodiscard]] bool is_load_target(Op op);

// True if the op is a conditional branch (a run barrier in the threaded
// lowering, so its marginal cost includes one extra enter_run).
[[nodiscard]] bool is_branch_target(Op op);

[[nodiscard]] std::string op_name(Op op);
[[nodiscard]] std::string context_name(OperandContext context);
[[nodiscard]] std::string working_set_name(WorkingSet working_set);
[[nodiscard]] std::string magnitude_name(OperandMagnitude magnitude);
[[nodiscard]] std::string pair_name(PairKind pair);

// True when the lowering is *expected* to fuse this pair, so the audit should
// require that it did rather than that it did not.
//
// Most substitutions must lower one slot per instruction, or the slope is not a
// per-instruction cost. A pair chosen precisely because the engine fuses it --
// auipc;jalr, the pc-relative call idiom -- inverts that: fusion is the thing
// being measured, and its absence would mean the sweep is measuring something
// else. Either way the audit checks the lowering against intent; only the
// direction of the expectation changes.
[[nodiscard]] bool pair_expects_fusion(PairKind pair);

// The two opcodes a pair substitution actually places.
//
// Every consumer that reasons about a pair in terms of its parts -- the gas it
// should be credited with, and the worst-case ceiling its interaction has to
// respect -- needs this, and each one hardcoding a guess is how the bound came
// to be checked against the wrong opcodes: the ceiling was written as SW+LW
// back when both pairs were a store and a load, and auipc;jalr inherited it
// silently. Derive it here so a new PairKind cannot be added without saying
// what it is made of.
[[nodiscard]] std::pair<Op, Op> pair_components(PairKind pair);

// Instructions one substitution places: 2 for a pair, 1 otherwise.
[[nodiscard]] uint32_t slots_per_target(const KernelSpec &spec);

// The most substitutions a body of `body_slots` can hold while keeping at least
// one filler between them. Shared so the sweep planner and the kernel builder
// cannot disagree: when they did, the densest kernel placed two pairs adjacent,
// the trailing load of one fused with the leading store of the next, and the
// audit rejected the sweep.
[[nodiscard]] uint32_t max_target_slots(uint32_t body_slots,
                                        const KernelSpec &spec);

// Gas the current in-tree schedule charges for one instance of `op`.
[[nodiscard]] uint64_t scheduled_gas_for(Op op);

// Total gas the current schedule charges for `code`, decoded with the same
// production decoder the engine uses. Returns nullopt if the code does not
// decode.
[[nodiscard]] std::expected<uint64_t, KernelError>
scheduled_gas_for_code(std::span<const uint8_t> code);

// The non-fusing filler instruction for body slot `index`: an R-type ADD whose
// destination rotates over the filler bank and whose sources the body never
// writes.
[[nodiscard]] uint32_t filler_instruction_word(uint32_t index);

// A canonical encoding of `op` with every register field set to `reg`.
//
// This is the opposite of what the calibration kernels want: pointing every
// register field at one register is what makes adjacent instructions satisfy
// the lowering's same-register fusion conditions (lui+addi and load-use both
// require the pair to share a register). That makes it the right shape for
// probing which pairs the engine fuses -- see engine_fingerprint() in
// provenance.h -- and the wrong shape for measuring anything.
//
// Returns nullopt for opcodes with no register-uniform encoding
// (JAL/JALR/FENCE/ECALL/EBREAK/UNIMP).
[[nodiscard]] std::expected<uint32_t, KernelError>
canonical_instruction_word(Op op, uint8_t reg);

// Materializes a 32-bit constant into `reg` (addi, or lui+addi when it does not
// fit a 12-bit immediate).
void emit_load_immediate(std::vector<uint32_t> &words, uint8_t reg,
                         int32_t value);

// Register setup shared by calibration kernels and validation workloads:
// counter, operand sources, data bank, load destinations, and the address bank
// placed for `working_set`. Ends on an `addi`, so a following R-type filler
// cannot fuse across the boundary.
void emit_prologue(std::vector<uint32_t> &words, WorkingSet working_set,
                   uint32_t trip_count);

// Builds the kernel, or the constraint the spec violated.
[[nodiscard]] std::expected<Kernel, KernelError>
build_kernel(const KernelSpec &spec);

} // namespace hogd::rv32::gas_calibration
