#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hogd::rv32 {
namespace {

inline constexpr int32_t kHostFnNoneOrRecoverable = 0;
inline constexpr int32_t kHostFnOutOfGas = 1;
inline constexpr int32_t kHostFnReturn = 3;
inline constexpr int32_t kHostFnExit = 4;

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
  store_result_footer(memory, args.a0, result);

  switch (result.panic) {
  case kHostFnNoneOrRecoverable:
    if (!view.write_memory(0, memory)) {
      return HostResult::trap();
    }
    return HostResult::continue_with(static_cast<uint32_t>(result.result_code));
  case kHostFnReturn:
    if (!view.write_memory(0, memory)) {
      return HostResult::trap();
    }
    return HostResult::halt();
  case kHostFnExit:
    if (!view.write_memory(0, memory)) {
      return HostResult::trap();
    }
    return HostResult::halt(static_cast<uint32_t>(result.result_code));
  case kHostFnOutOfGas:
    return HostResult::out_of_gas();
  default:
    if (!view.write_memory(0, memory)) {
      return HostResult::trap();
    }
    return HostResult::trap();
  }
}

} // namespace hogd::rv32
