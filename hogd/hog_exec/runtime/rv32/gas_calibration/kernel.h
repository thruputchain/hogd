#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/program.h"

// Deterministic instruction kernels. Saturated bodies provide absolute costs;
// substitution sweeps vary target count in a fixed body as a diagnostic.
// The ADD filler uses private registers; lowering_audit.h checks for unintended
// fusion. Saturated single-opcode bodies are lowered with fusion disabled.
namespace hogd::rv32::gas_calibration {

// Separate target, filler, and loop-control registers preserve the requested
// dependencies. Filler destinations rotate, with read-only sources, so the
// baseline does not acquire a latency chain of its own.
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
// Divide by one keeps a dependent chain from converging to the zero fast path.
inline constexpr uint8_t kOneReg = 30;

// Operand dependencies expose latency as well as throughput.
enum class OperandContext : uint8_t {
  // Every target writes and reads one register: a serial dependence chain, so
  // the target's own latency is exposed.
  kChain,
  // Targets rotate over an 8-register bank: no target-to-target dependence,
  // so the measurement is throughput-bound.
  kIndependent,
  // Direct jump without writing a return address (JAL x0).
  kNoLink,
  // JALR repeatedly dispatches from one PC to targets read from a linked table.
  kTargetChase,
};

// Memory contexts use the default guest memory size. They do not cover the
// larger memory sizes permitted by configured runtime limits.
enum class WorkingSet : uint8_t {
  kNone,      // non-memory target
  kL1Region,  // all bases inside one 4 KiB region
  kFullRange, // bases spread across the full guest memory
  // Dependent loads through a shuffled address cycle. Wide loads carry
  // pointers; byte loads use a restricted cycle. See target_word().
  kPointerChase,
  // Repeated writes to one address exercise store-buffer contention.
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

[[nodiscard]] bool is_divide_target(Op op);

// Adjacent pairs measure interactions absent from isolated opcode costs.
// Store/load cases differ only in address overlap, isolating forwarding.
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
  // Fill interior slots with targets. Single-opcode bodies disable fusion;
  // pair bodies retain it. run_sweep selects this and the audit verifies it.
  bool saturated = false;
  // Vary placement at a fixed count to expose layout-dependent timing bias.
  uint32_t placement_phase = 0;
  // Loop iterations. Chosen by the driver so each kernel runs long enough to
  // dominate timer overhead.
  uint32_t trip_count = 64;
};

struct Kernel {
  std::vector<uint8_t> code;
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = kDefaultMaxMemoryBytes;
  // Instructions retired per loop iteration and over the whole run. Used to
  // cross-check the measurement against what actually executed.
  uint64_t instructions_per_iteration = 0;
  uint64_t dynamic_instructions = 0;
  // Gas the current schedule charges for the whole run.
  uint64_t scheduled_gas = 0;
  // Byte offsets of the body slots that hold the target instruction, in
  // program order. lowering_audit.h uses these to check the lowering.
  std::vector<uint32_t> target_slot_pcs;
  // Timing denominator: instructions for a single target, pairs for a pair
  // target. target_slot_pcs contains two entries per pair.
  uint32_t target_units = 0;
  // Byte offset of the first body slot.
  uint32_t body_start_pc = 0;
};

enum class KernelError {
  kUnsupportedTarget, // no body-slot encoding for this opcode
  kBodyTooSmall,      // fewer slots than the fixed boundaries need
  kZeroTripCount,
  kTooManyTargets, // would put two targets adjacent, exposing them to fusion
  kCodeTooLarge,   // over kDefaultMaxCodeBytes
  // A body slot's own immediate does not reach. JALR carries the whole
  // per-slot offset from a base anchored at the body midpoint, so its 12-bit
  // signed field caps how large a body it can fill.
  kTargetImmediateOutOfRange,
  kUndecodable, // generated code did not round-trip through the decoder
};

[[nodiscard]] std::string kernel_error_name(KernelError error);

// True for opcodes this harness knows how to place in a body slot. The
// trapping/halting ops (ECALL, EBREAK, UNIMP) and FENCE are excluded. JAL/JALR
// body-slot encodings remain useful for correctness tests, but their timings
// now come exclusively from build_jump_kernel's control-flow graphs.
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

// Pair measurements can require fusion instead of rejecting it.
[[nodiscard]] bool pair_expects_fusion(PairKind pair);

// Shared by instruction counting and the per-pair cost bound.
[[nodiscard]] std::pair<Op, Op> pair_components(PairKind pair);

// Instructions one substitution places: 2 for a pair, 1 otherwise.
[[nodiscard]] uint32_t slots_per_target(const KernelSpec &spec);

// Maximum substitutions with a filler between them. Shared by the planner
// and builder so adjacent pairs cannot introduce unintended fusion.
[[nodiscard]] uint32_t max_target_slots(uint32_t body_slots,
                                        const KernelSpec &spec);

// Gas the current in-tree schedule charges for one instance of `op`.
[[nodiscard]] uint64_t scheduled_gas_for(Op op);

// Total gas the current schedule charges for `code`, decoded with the same
// production decoder the engine uses. Returns an error if decoding fails.
[[nodiscard]] std::expected<uint64_t, KernelError>
scheduled_gas_for_code(std::span<const uint8_t> code);

// The non-fusing filler instruction for body slot `index`: an R-type ADD whose
// destination rotates over the filler bank and whose sources the body never
// writes.
[[nodiscard]] uint32_t filler_instruction_word(uint32_t index);

// Uniform register fields expose same-register fusion rules for fingerprint
// probes. Returns an error for opcodes without a register-uniform encoding.
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

enum class JumpLayout : uint8_t { kSequential, kReverse, kShuffled };

struct JumpSpec {
  Op target = Op::kJal;
  OperandContext context = OperandContext::kIndependent;
  JumpLayout layout = JumpLayout::kShuffled;
  uint32_t nodes = 256;
  uint32_t stride_words = 1;
  uint32_t seed = 1;
  uint32_t trip_count = 64;
};

struct JumpKernel {
  Kernel kernel;
  // Exact PC sequence for one iteration, including setup and loop control.
  // The final iteration omits the last (back-edge) instruction.
  std::vector<uint32_t> iteration_pcs;
  uint32_t loop_pc = 0;
  uint32_t exit_pc = 0;
};

// Pure direct jumps, fixed-base indirect jumps, link-dependent indirect jumps,
// or a shared JALR dispatcher whose target changes at every visit. Unvisited
// padding traps, so a broken edge cannot silently retire filler instructions.
[[nodiscard]] std::expected<JumpKernel, KernelError>
build_jump_kernel(const JumpSpec &spec);
[[nodiscard]] std::string jump_context_name(const JumpSpec &spec);

} // namespace hogd::rv32::gas_calibration
