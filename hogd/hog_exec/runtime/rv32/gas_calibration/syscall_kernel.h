#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/program.h"

// Synthetic ECALL kernels measure suspend, Host dispatch, and resume, with
// optional output writes. They exclude production syscall_entry parsing,
// pointer checks, coroutine allocation, and handler work. The separate
// rv32_syscall_entry_bench measures that path; see README.md.
namespace hogd::rv32::gas_calibration {

inline constexpr uint32_t kEcallWord = 0x00000073U;

// Keep fillers outside a0-a7. The opcode kernel's x14-x17 destinations would
// overwrite syscall arguments between calls.
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

// Selectors must fit ADDI's signed immediate; the encoder masks excess bits.
static_assert(kSyscallContinueSelector <= 0x7FF,
              "continue selector must fit addi's signed 12-bit immediate");
static_assert(kSyscallHaltSelector <= 0x7FF,
              "halt selector must fit addi's signed 12-bit immediate");
static_assert(kSyscallContinueSelector != kSyscallHaltSelector,
              "the terminal call must be distinguishable from a body call");

// Reuse fixed output addresses, modelling repeated writes to a guest stack slot.
inline constexpr uint32_t kSyscallOutPointerA = 0x0400;
inline constexpr uint32_t kSyscallOutPointerB = 0x0800;

// Shapes other than kSeam share the guest instruction sequence.
enum class SyscallShape : uint8_t {
  // Echo a0 to preserve the selector across calls and isolate the crossing.
  // Production calls return status in a0; use kSelectorReload for that ABI.
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

// Counts body/terminal calls and performs each shape's synthetic host work.
class SyscallDispatchHost final : public Host {
public:
  explicit SyscallDispatchHost(SyscallShape shape) : shape_(shape) {}

  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override;

  // Includes the terminal halt; checked against the kernel's expected count.
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
  // Fill the body with calls for the absolute per-call timing estimate.
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
  // Instruction gas only: this synthetic host returns zero host gas.
  uint64_t scheduled_gas = 0;
  uint32_t body_start_pc = 0;
};

// Capacity between fixed filler boundaries. ECALL groups may be adjacent:
// ECALL is a barrier and cannot fuse.
[[nodiscard]] uint32_t max_call_groups(uint32_t body_slots, SyscallShape shape);

[[nodiscard]] std::expected<SyscallKernel, KernelError>
build_syscall_kernel(const SyscallKernelSpec &spec);

} // namespace hogd::rv32::gas_calibration
