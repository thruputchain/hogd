#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "syscall_error.hh"

namespace hogd::rv32 {
namespace {

uint64_t
load_le64(std::span<const uint8_t> bytes, size_t offset)
{
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    value |= uint64_t{bytes[offset + i]} << (i * 8);
  }
  return value;
}

void
store_le32(std::span<uint8_t> bytes, size_t offset, uint32_t value)
{
  for (size_t i = 0; i < sizeof(uint32_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void
store_le64(std::span<uint8_t> bytes, size_t offset, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void
store_result_footer(std::span<uint8_t> memory, uint32_t frame_address,
                    const TrampolineResult &result)
{
  const size_t offset = size_t{frame_address} + kSyscallFrameResultOffset;
  store_le32(memory, offset, static_cast<uint32_t>(result.result_code));
  store_le32(memory, offset + sizeof(uint32_t),
             static_cast<uint32_t>(result.panic));
  store_le64(memory, offset + 2 * sizeof(uint32_t), result.gas_consumed);
}

} // namespace

SyscallBridgeHost::SyscallBridgeHost(ExternHostCallContext context,
                                     HostFn11 entry)
  : context_(context), entry_(entry)
{}

HostResult
SyscallBridgeHost::ecall(MachineView view, const EcallArgs &args) noexcept
{
  last_result_.reset();
  if (entry_ == nullptr || args.a7 != kSyscallBridgeVersion ||
      args.a0 % alignof(uint64_t) != 0) {
    return HostResult::trap();
  }

  // M2 bridge ABI: a0 points at 11 little-endian u64 words matching
  // syscall_entry(arg0..arg10), followed by a TrampolineResult footer. The
  // scratch copy keeps MachineView narrow while still exercising the same
  // mutable WasmContext.memory shape used by the existing syscall parser.
  std::array<uint8_t, kSyscallFrameBytes> frame = {};
  if (!view.read_memory(args.a0, frame)) {
    return HostResult::trap();
  }

  std::array<uint64_t, kSyscallFrameArgWords> words = {};
  for (size_t i = 0; i < words.size(); ++i) {
    words[i] = load_le64(frame, i * sizeof(uint64_t));
  }

  const std::span<const uint8_t> view_memory = view.memory();
  std::vector<uint8_t> scratch;
  try {
    scratch.assign(view_memory.begin(), view_memory.end());
  } catch (...) {
    return HostResult::trap();
  }
  const std::span<uint8_t> memory(scratch.data(), scratch.size());
  WasmContext wasm_context{
      .memory = LinearMemorySlice{.mem = memory.data(), .sz = memory.size()},
      .gas_remaining = view.gas_remaining(),
  };

  TrampolineResult result = {};
  try {
    result = entry_(context_, wasm_context, words[0], words[1], words[2],
                    words[3], words[4], words[5], words[6], words[7], words[8],
                    words[9], words[10]);
  } catch (...) {
    return HostResult::trap();
  }
  last_result_ = result;

  switch (result.panic) {
  case std::to_underlying(HostFnErrorCode::NoneOrRecoverable):
    // Fault means the parser/output layer rejected guest memory. Preserve the
    // return code through a0, but do not commit scratch memory or the footer.
    if (result.result_code ==
        std::to_underlying(SyscallErrorCode::kSyscallErrorCodeFault)) {
      return HostResult::continue_with(static_cast<uint32_t>(
                                           result.result_code),
                                       result.gas_consumed);
    }
    store_result_footer(memory, args.a0, result);
    if (!view.write_memory(0, memory)) {
      return HostResult::trap(result.gas_consumed);
    }
    return HostResult::continue_with(static_cast<uint32_t>(result.result_code),
                                     result.gas_consumed);
  case std::to_underlying(HostFnErrorCode::Return):
    // Return and Exit terminate the VM invocation cleanly after committing the
    // same scratch/footer writes the WASM path would expose.
    store_result_footer(memory, args.a0, result);
    if (!view.write_memory(0, memory)) {
      return HostResult::trap(result.gas_consumed);
    }
    return HostResult::halt(0, result.gas_consumed);
  case std::to_underlying(HostFnErrorCode::Exit):
    store_result_footer(memory, args.a0, result);
    if (!view.write_memory(0, memory)) {
      return HostResult::trap(result.gas_consumed);
    }
    return HostResult::halt(static_cast<uint32_t>(result.result_code),
                            result.gas_consumed);
  case std::to_underlying(HostFnErrorCode::OutOfGas):
    // OOG is atomic with respect to guest memory: Machine::complete_ecall
    // charges reported host gas, then rolls back the pre-host snapshot.
    return HostResult::out_of_gas(result.gas_consumed);
  default:
    // Unknown or fatal host panics trap without committing scratch memory.
    return HostResult::trap(result.gas_consumed,
                            static_cast<uint32_t>(result.panic));
  }
}

} // namespace hogd::rv32
