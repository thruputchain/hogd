#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/program.h"

// What the engine pays to leave the guest, run a host call, and come back.
//
// This is the *seam*, not a syscall. `gas_cost_table.h` prices one syscall
// family (SHA256) and charges zero direct host gas for the rest, so the cost of
// a stateless syscall today is whatever the crossing itself costs plus whatever
// the handler does -- and nothing has ever measured the first half. That half
// is the same for every syscall, which is what makes it worth measuring on its
// own: it is the constant floor under all of them.
//
// WHAT IS AND IS NOT IN THESE NUMBERS
//
// In: op_ecall's suspend (spill gas, stash a0-a7, yield kEcall), the unwind to
// the drive() frame, the virtual dispatch through Host::ecall, apply_ecall's
// gas debit and register commit, and the fresh enter_run the resumed guest
// re-enters through. Every one of those is paid by every syscall the node
// makes, and no other harness in the tree measures them.
//
// Out, and this is the part to state loudly: the production syscall path
// itself. `syscall_entry` parses the register-native ABI through
// SyscallArgParams, bounds-checks guest pointers, allocates a coroutine frame
// (runtime_exports.h: "Always the Async arm today: wrap_after_parse_async() is
// a coroutine unconditionally, so every syscall allocates a frame"), and calls
// into SystemCallHandler. None of that is here, because none of it can be:
// `syscall_entry` needs the protocol XDR, the DB proxy and the snapshot cache,
// and this harness links only rv32_isa_core -- deliberately, so a calibration
// can be run on a candidate validator without configuring the node build.
//
// So read these as a LOWER BOUND on any syscall's fixed cost, and read the
// difference between the shapes below as the shape of what the guest ABI costs
// on top. A number that claims to be "the syscall base cost" is not in this
// file; producing one needs a second, runtime-linked harness, and the point of
// measuring the seam first is that its cost is then already accounted for.
namespace hogd::rv32::gas_calibration {

// The ecall instruction word. kernel.cc spells this literal inline for the
// single terminal ecall it emits; a body made of them wants it named.
inline constexpr uint32_t kEcallWord = 0x00000073U;

// Register plan. Deliberately NOT the opcode kernel's plan, and the reason is
// a hazard rather than tidiness: the syscall ABI is a0-a7 = x10-x17, and
// kFillerDstBase..+kFillerDstSize in kernel.h is x14-x17. An opcode-kernel
// filler placed between two ecalls would rewrite a4-a7 between one call and
// the next, so the arguments the host sees would depend on how many filler
// slots preceded it. Every register this harness uses is therefore chosen to
// miss x10-x17.
inline constexpr uint8_t kSyscallArgBase = 10; // a0..a7 = x10..x17
inline constexpr uint8_t kSyscallArgCount = 8;
inline constexpr uint8_t kSyscallLoopCounterReg = 2;
// Filler sources, never written by the body.
inline constexpr uint8_t kSyscallFillerSrcA = 1;
inline constexpr uint8_t kSyscallFillerSrcB = 3;
// Rotating filler destinations, above the argument registers.
inline constexpr uint8_t kSyscallFillerDstBase = 18;
inline constexpr uint8_t kSyscallFillerDstSize = 4; // x18..x21

// Selector the guest leaves in a0 for a call the host should return from.
// Any value but kSyscallHaltSelector does; the number is not a real SyscallNo
// and is not meant to look like one -- no production syscall table is reachable
// from here.
inline constexpr uint32_t kSyscallContinueSelector = 0x21;
// Selector for the one ecall that ends the run. The body cannot halt, so the
// terminal ecall has to be distinguishable from the ones being measured, and a
// selector in a0 is how a real guest already distinguishes calls.
inline constexpr uint32_t kSyscallHaltSelector = 0x7FF;

// Both selectors are materialized with a single `addi rd, x0, imm`, whose
// immediate is 12 bits SIGNED. The encoder masks rather than refuses, so a
// selector above 2047 encodes as a negative number, the host never matches it,
// and the kernel runs off the end of its code instead of halting -- with the
// call count still looking exactly right, because the terminal call was made.
// Cost one debugging session before this assert existed.
static_assert(kSyscallContinueSelector <= 0x7FF,
              "continue selector must fit addi's signed 12-bit immediate");
static_assert(kSyscallHaltSelector <= 0x7FF,
              "halt selector must fit addi's signed 12-bit immediate");
static_assert(kSyscallContinueSelector != kSyscallHaltSelector,
              "the terminal call must be distinguishable from a body call");

// Guest addresses the out-pointer shapes write through. A real stateless read
// syscall writes its result into a caller stack slot, so the same address every
// call is the realistic arrangement, not a cache-friendly cheat.
inline constexpr uint32_t kSyscallOutPointerA = 0x0400;
inline constexpr uint32_t kSyscallOutPointerB = 0x0800;

// What the host does per call, chosen to span the host-side shapes the
// genuinely stateless syscalls actually have. The guest side is identical for
// every shape but kSeam, so the differences between the other four are host
// work and nothing else.
enum class SyscallShape : uint8_t {
  // ecall alone, and the host echoes a0 back so the selector survives its own
  // return value. Nothing else: no argument reload, no memory touched. This is
  // the isolated seam and the floor under every row below it.
  //
  // It is also the one shape that is not a legal guest sequence. The bridge ABI
  // returns the syscall's status in a0 (apply_ecall: `regs_[10] =
  // result.continue_value()`), so a real guest must re-materialize the selector
  // before each call. kSeam suppresses that by having the host hand the
  // selector back, which isolates the crossing at the cost of describing a
  // caller that cannot exist. Compare it against kSelectorReload to see what
  // the ABI's own overhead is; quote it on its own only as a bound.
  kSeam,
  // addi a0, x0, selector ; ecall -- the real ABI's minimum. The host returns
  // status 0, clobbering a0, and the guest reloads it. This is the cheapest
  // sequence a conforming guest can use to make a syscall twice.
  kSelectorReload,
  // As kSelectorReload, and the host bounds-checks a1 and writes 8 bytes
  // through it. The shape of GetBlockHeight, whose guest signature is one out
  // pointer to a 64-bit height (sdk_rs: `raw_GetBlockHeight(&mut block)`).
  kOutPointer8,
  // As kOutPointer8 with a 32-byte write: the address- and hash-returning
  // reads (GetCurrentAddress, GetMsgSender, GetSourceTransactionHash).
  kOutPointer32,
  // Two out pointers, four bytes each, the shape of GetCalldataLen
  // (`raw_GetCalldataLen(&mut len, &mut auth_token_count)`). Present to show
  // whether a second bounds-checked write is visible against the crossing at
  // all -- if it is not, output count is not a term worth pricing.
  kTwoOutPointers,
};

inline constexpr std::array<SyscallShape, 5> kAllSyscallShapes = {
    SyscallShape::kSeam, SyscallShape::kSelectorReload,
    SyscallShape::kOutPointer8, SyscallShape::kOutPointer32,
    SyscallShape::kTwoOutPointers};

[[nodiscard]] std::string syscall_shape_name(SyscallShape shape);

// One-line statement of the real syscall whose host-side shape this models, for
// the report. Not a claim that the syscall costs this: the handler is not here.
[[nodiscard]] std::string syscall_shape_models(SyscallShape shape);

// Guest instructions one call group occupies: 1 for kSeam, 2 for the rest.
[[nodiscard]] uint32_t syscall_shape_slots(SyscallShape shape);

// Bytes the host writes into guest memory per call.
[[nodiscard]] uint32_t syscall_shape_output_bytes(SyscallShape shape);

// The host under measurement: performs the shape's host-side work, counts the
// calls it served, and halts on the terminal selector.
//
// It is a Host subclass reached through a virtual call, exactly as
// SyscallBridgeHost is in production, so the dispatch this measures is the
// dispatch the node pays.
class SyscallDispatchHost final : public Host {
public:
  explicit SyscallDispatchHost(SyscallShape shape) : shape_(shape) {}

  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override;

  // Host calls served since construction, the terminal halt included. The
  // harness checks this against what the kernel says it should be: a body that
  // executed a different number of calls than it was built to would be timed
  // against the wrong denominator, and nothing else would notice.
  [[nodiscard]] uint64_t calls() const { return calls_; }
  void reset_calls() { calls_ = 0; }

  // Set when a bounds-checked write through an out pointer failed. A faulted
  // write does less work than a real one, so a run that faults must be thrown
  // away rather than reported as a cheap call.
  [[nodiscard]] bool faulted() const { return faulted_; }

private:
  SyscallShape shape_;
  uint64_t calls_ = 0;
  bool faulted_ = false;
};

struct SyscallKernelSpec {
  SyscallShape shape = SyscallShape::kSelectorReload;
  // Body slot count, constant across a sweep.
  uint32_t body_slots = 512;
  // How many call groups the body holds. The rest of the body is filler.
  uint32_t call_groups = 0;
  // Fill the body with call groups back to back. The saturated body is the
  // primary estimator here for the same reason it is for opcodes: its time
  // over its call count is directly what a loop of syscalls costs per call,
  // with no filler cost to subtract and no way to come out negative.
  bool saturated = false;
  uint32_t trip_count = 64;
};

struct SyscallKernel {
  std::vector<uint8_t> code;
  std::vector<uint8_t> initial_memory;
  uint32_t memory_bytes = kDefaultMaxMemoryBytes;
  // Call groups placed in the body: the denominator a saturated timing divides
  // by, and the unit its cost is quoted in (ns per host call).
  uint32_t calls_per_iteration = 0;
  uint64_t instructions_per_iteration = 0;
  uint64_t dynamic_instructions = 0;
  // Host calls the whole run should make: the body's, plus the terminal halt.
  uint64_t expected_host_calls = 0;
  // Gas the current schedule charges for the whole run. Instruction gas only:
  // these shapes model syscalls that charge no direct host gas today, which is
  // itself the finding the numbers speak to.
  uint64_t scheduled_gas = 0;
  uint32_t body_start_pc = 0;
};

// The most call groups a body of `body_slots` can hold. Unlike an opcode
// sweep there is no non-adjacency cap: ecall is a run barrier and fuses with
// nothing, so adjacent call groups cannot create or destroy a fusion the way
// adjacent ALU ops can. The bound is just how many groups fit between the two
// fixed filler boundaries.
[[nodiscard]] uint32_t max_call_groups(uint32_t body_slots, SyscallShape shape);

[[nodiscard]] std::expected<SyscallKernel, KernelError>
build_syscall_kernel(const SyscallKernelSpec &spec);

} // namespace hogd::rv32::gas_calibration
