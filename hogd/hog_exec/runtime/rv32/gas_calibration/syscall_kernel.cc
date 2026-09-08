#include "hogd/hog_exec/runtime/rv32/gas_calibration/syscall_kernel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"

namespace hogd::rv32::gas_calibration {
namespace {

using test_program::addi;
using test_program::branch;
using test_program::j_type;
using test_program::op;

// Argument register n, as an x-register index.
constexpr uint8_t
arg_reg(uint8_t index)
{
  return static_cast<uint8_t>(kSyscallArgBase + index);
}

void
append_u32(std::vector<uint8_t> &out, uint32_t word)
{
  out.push_back(static_cast<uint8_t>(word & 0xFFU));
  out.push_back(static_cast<uint8_t>((word >> 8) & 0xFFU));
  out.push_back(static_cast<uint8_t>((word >> 16) & 0xFFU));
  out.push_back(static_cast<uint8_t>((word >> 24) & 0xFFU));
}

// Filler for the slots a sweep leaves without a call group: an R-type ADD whose
// destination rotates over x18..x21 and whose sources the body never writes.
// Rotating rather than chaining, for the reason kernel.h gives at length -- a
// filler that chains through one register makes the all-filler point
// latency-bound and the sweep's low end stops describing dispatch throughput.
//
// Fusion is not a concern the way it is for opcode kernels: ecall is a run
// barrier and matches no fusion rule in either position, so a call group's
// neighbours cannot change what it lowers to. add;add is not fused either, so
// the filler runs do not collapse.
uint32_t
syscall_filler_word(uint32_t index)
{
  const auto dst = static_cast<uint8_t>(kSyscallFillerDstBase +
                                        (index % kSyscallFillerDstSize));
  return op(dst, kSyscallFillerSrcA, kSyscallFillerSrcB, 0, 0);
}

// The words one call group places, in order. Every shape but kSeam reloads the
// selector first, because apply_ecall writes the host's return value into a0
// and a conforming guest therefore cannot reuse it.
std::vector<uint32_t>
call_group_words(SyscallShape shape)
{
  if (shape == SyscallShape::kSeam) {
    return {kEcallWord};
  }
  return {addi(arg_reg(0), 0, static_cast<int32_t>(kSyscallContinueSelector)),
          kEcallWord};
}

// Registers, then the loop counter. a1 and a2 carry the out pointers the host
// writes through; a3..a7 carry constants so the host reads a plausible argument
// block rather than zeros it might be able to shortcut. Nothing in the body
// writes them, and apply_ecall commits only a0, so they hold for the whole run.
void
emit_syscall_prologue(std::vector<uint32_t> &words, uint32_t trip_count)
{
  emit_load_immediate(words, kSyscallLoopCounterReg,
                      static_cast<int32_t>(trip_count));
  emit_load_immediate(words, kSyscallFillerSrcA, 0x00BEEF);
  emit_load_immediate(words, kSyscallFillerSrcB, 0x000101);
  for (uint32_t i = 0; i < kSyscallFillerDstSize; ++i) {
    emit_load_immediate(words, static_cast<uint8_t>(kSyscallFillerDstBase + i),
                        1);
  }
  emit_load_immediate(words, arg_reg(0),
                      static_cast<int32_t>(kSyscallContinueSelector));
  emit_load_immediate(words, arg_reg(1),
                      static_cast<int32_t>(kSyscallOutPointerA));
  emit_load_immediate(words, arg_reg(2),
                      static_cast<int32_t>(kSyscallOutPointerB));
  for (uint8_t i = 3; i < kSyscallArgCount; ++i) {
    emit_load_immediate(words, arg_reg(i), static_cast<int32_t>(0x40 + i));
  }
}

} // namespace

std::string
syscall_shape_name(SyscallShape shape)
{
  switch (shape) {
  case SyscallShape::kSeam:
    return "seam";
  case SyscallShape::kSelectorReload:
    return "selector_reload";
  case SyscallShape::kOutPointer8:
    return "out_pointer_8";
  case SyscallShape::kOutPointer32:
    return "out_pointer_32";
  case SyscallShape::kTwoOutPointers:
    return "two_out_pointers";
  }
  return "unknown";
}

std::string
syscall_shape_models(SyscallShape shape)
{
  switch (shape) {
  case SyscallShape::kSeam:
    return "the ecall crossing alone; not a legal guest sequence";
  case SyscallShape::kSelectorReload:
    return "the bridge ABI's minimum: reload a0, call, take status back";
  case SyscallShape::kOutPointer8:
    return "GetBlockHeight (one out pointer, 8 bytes)";
  case SyscallShape::kOutPointer32:
    return "GetCurrentAddress / GetMsgSender (one out pointer, 32 bytes)";
  case SyscallShape::kTwoOutPointers:
    return "GetCalldataLen (two out pointers, 4 bytes each)";
  }
  return "unknown";
}

uint32_t
syscall_shape_slots(SyscallShape shape)
{
  return shape == SyscallShape::kSeam ? 1U : 2U;
}

uint32_t
syscall_shape_output_bytes(SyscallShape shape)
{
  switch (shape) {
  case SyscallShape::kSeam:
  case SyscallShape::kSelectorReload:
    return 0;
  case SyscallShape::kOutPointer8:
    return 8;
  case SyscallShape::kOutPointer32:
    return 32;
  case SyscallShape::kTwoOutPointers:
    return 8; // two four-byte writes
  }
  return 0;
}

HostResult
SyscallDispatchHost::ecall(MachineView view, const EcallArgs &args) noexcept
{
  ++calls_;
  if (args.a0 == kSyscallHaltSelector) {
    return HostResult::halt(0);
  }

  // Payload derived from the call counter so the writes cannot be hoisted or
  // folded into one store by a compiler that can see the whole host.
  std::array<uint8_t, 32> payload = {};
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(calls_ + i);
  }

  switch (shape_) {
  case SyscallShape::kSeam:
    // Hand the selector back, so the guest does not have to reload it. See the
    // header: this is what makes kSeam an isolate rather than a caller.
    return HostResult::continue_with(args.a0);
  case SyscallShape::kSelectorReload:
    break;
  case SyscallShape::kOutPointer8:
    if (!view.write_memory(args.a1, std::span(payload).first(8))) {
      faulted_ = true;
    }
    break;
  case SyscallShape::kOutPointer32:
    if (!view.write_memory(args.a1, std::span(payload))) {
      faulted_ = true;
    }
    break;
  case SyscallShape::kTwoOutPointers:
    if (!view.write_memory(args.a1, std::span(payload).first(4)) ||
        !view.write_memory(args.a2, std::span(payload).subspan(4, 4))) {
      faulted_ = true;
    }
    break;
  }
  // The bridge returns the syscall's status in a0; zero is success. Every shape
  // but kSeam therefore clobbers the selector, which is what forces the reload
  // its call group pays for.
  return HostResult::continue_with(0);
}

uint32_t
max_call_groups(uint32_t body_slots, SyscallShape shape)
{
  // Slot 0 and the last slot hold filler, pinning both body boundaries for
  // every kernel in a sweep exactly as build_kernel does.
  if (body_slots < 4) {
    return 0;
  }
  return (body_slots - 2) / syscall_shape_slots(shape);
}

std::expected<SyscallKernel, KernelError>
build_syscall_kernel(const SyscallKernelSpec &spec)
{
  if (spec.body_slots < 8) {
    return std::unexpected(KernelError::kBodyTooSmall);
  }
  if (spec.trip_count == 0) {
    return std::unexpected(KernelError::kZeroTripCount);
  }
  const uint32_t per_group = syscall_shape_slots(spec.shape);
  const uint32_t cap = max_call_groups(spec.body_slots, spec.shape);
  const uint32_t groups = spec.saturated ? cap : spec.call_groups;
  if (groups > cap) {
    return std::unexpected(KernelError::kTooManyTargets);
  }

  std::vector<uint32_t> prologue;
  emit_syscall_prologue(prologue, spec.trip_count);

  std::vector<uint32_t> body(spec.body_slots);
  for (uint32_t slot = 0; slot < spec.body_slots; ++slot) {
    body[slot] = syscall_filler_word(slot);
  }

  // Even placement across the interior, the same spread build_kernel uses:
  // clustering the groups at one end would give the body a call-dense half and
  // a filler-dense half, which run at different rates, and the point would
  // report their average as though it were one rate.
  //
  // Placed by ideal start position with a monotone clamp rather than by
  // build_kernel's per-slot Bresenham, because a group is per_group slots wide
  // and the per-slot form has no way to say that. Left as-is it packs the
  // saturated body one slot short and rejects it: at the cap the ideal
  // positions are exactly per_group apart, so every one of them has to land
  // where it was asked to.
  const uint32_t interior = spec.body_slots - 2;
  const std::vector<uint32_t> group_words = call_group_words(spec.shape);
  uint32_t next_free = 1;
  for (uint32_t i = 0; i < groups; ++i) {
    const uint32_t ideal = 1 + ((i * interior) / groups);
    // A group occupies per_group consecutive slots, so a start that would
    // overlap the previous group's tail is pushed past it rather than allowed
    // to overwrite its ecall -- which would place fewer calls than the
    // denominator says while leaving nothing to notice it by.
    const uint32_t slot = std::max(ideal, next_free);
    if (slot + per_group > spec.body_slots - 1) {
      return std::unexpected(KernelError::kTooManyTargets);
    }
    for (uint32_t w = 0; w < per_group; ++w) {
      body[slot + w] = group_words[w];
    }
    next_free = slot + per_group;
  }

  std::vector<uint32_t> words = prologue;
  const auto body_start_index = static_cast<uint32_t>(words.size());
  words.insert(words.end(), body.begin(), body.end());
  words.push_back(addi(kSyscallLoopCounterReg, kSyscallLoopCounterReg, -1));
  // beq counter, x0, +8 -- skip the JAL and fall into the terminal call.
  words.push_back(branch(0, kSyscallLoopCounterReg, 0,
                         2 * static_cast<int32_t>(kInstructionBytes)));
  const int32_t back_edge =
      -static_cast<int32_t>((spec.body_slots + 2) * kInstructionBytes);
  words.push_back(j_type(0, back_edge));
  // The one ecall that ends the run. A body ecall cannot halt -- it is the
  // thing being measured -- so the terminal call is told apart by its selector,
  // the same way a real guest tells its syscalls apart.
  words.push_back(
      addi(arg_reg(0), 0, static_cast<int32_t>(kSyscallHaltSelector)));
  words.push_back(kEcallWord);

  if (words.size() * kInstructionBytes > kMaxCodeBytes) {
    return std::unexpected(KernelError::kCodeTooLarge);
  }

  SyscallKernel kernel;
  kernel.code.reserve(words.size() * kInstructionBytes);
  for (const uint32_t word : words) {
    append_u32(kernel.code, word);
  }
  kernel.memory_bytes = kMaxMemoryBytes;
  kernel.initial_memory.assign(kMaxMemoryBytes, 0);
  kernel.body_start_pc = body_start_index * kInstructionBytes;
  kernel.calls_per_iteration = groups;
  kernel.instructions_per_iteration = spec.body_slots + 3;
  kernel.dynamic_instructions =
      prologue.size() + (uint64_t{spec.trip_count} * (spec.body_slots + 2)) +
      (uint64_t{spec.trip_count} - 1) + 2;
  kernel.expected_host_calls =
      (uint64_t{spec.trip_count} * groups) + 1; // + the terminal halt

  // Charge by decoding with the production decoder rather than by reasoning
  // about which slot holds what -- the same discipline build_kernel uses, and
  // the reason a gas total here can be compared against what the engine
  // actually charged.
  const std::vector<uint8_t> body_code = test_program::code_from_words(body);
  const auto body_gas = scheduled_gas_for_code(body_code);
  if (!body_gas.has_value()) {
    return std::unexpected(body_gas.error());
  }
  const std::vector<uint8_t> prologue_code =
      test_program::code_from_words(prologue);
  const auto prologue_gas = scheduled_gas_for_code(prologue_code);
  if (!prologue_gas.has_value()) {
    return std::unexpected(prologue_gas.error());
  }
  const uint64_t loop_gas =
      scheduled_gas_for(Op::kAddi) + scheduled_gas_for(Op::kBeq);
  kernel.scheduled_gas =
      *prologue_gas + (uint64_t{spec.trip_count} * (*body_gas + loop_gas)) +
      ((uint64_t{spec.trip_count} - 1) * scheduled_gas_for(Op::kJal)) +
      scheduled_gas_for(Op::kAddi) + scheduled_gas_for(Op::kEcall);
  return kernel;
}

} // namespace hogd::rv32::gas_calibration
