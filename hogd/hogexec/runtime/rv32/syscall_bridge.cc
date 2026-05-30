#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "syscall_error.hh"

namespace hogd::rv32 {

SyscallBridgeHost::SyscallBridgeHost(ExternHostCallContext context,
                                     HostFn8 entry)
  : context_(context), entry_(entry)
{}

HostResult
SyscallBridgeHost::ecall(MachineView view, const EcallArgs &args) noexcept
{
  last_result_.reset();
  if (entry_ == nullptr) {
    return HostResult::trap();
  }

  // Register-native bridge ABI: syscall_entry(arg0..arg7) is fed directly from
  // a0..a7 (a0 is the syscall number). The shared syscall ABI consumes 32-bit
  // slots, so no widening or sign-extension shim is needed here. The syscall
  // ABI version is checked at artifact load time, so no per-call version
  // register is needed.
  //
  // syscall_entry mutates WasmContext.memory in place, so run it against a
  // scratch copy of guest data memory. A parser/output fault can then be
  // rejected without committing partial writes, matching the WASM path's
  // atomicity, and the scratch is committed back only on success.
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
    result = entry_(context_, wasm_context, args.a0, args.a1, args.a2, args.a3,
                    args.a4, args.a5, args.a6, args.a7);
  } catch (...) {
    return HostResult::trap();
  }
  last_result_ = result;

  switch (result.panic) {
  case std::to_underlying(HostFnErrorCode::NoneOrRecoverable):
    // Fault means the parser/output layer rejected guest memory. Surface the
    // return code through a0 without committing scratch memory.
    if (result.result_code ==
        std::to_underlying(SyscallErrorCode::kSyscallErrorCodeFault)) {
      return HostResult::continue_with(static_cast<uint32_t>(
                                           result.result_code),
                                       result.gas_consumed);
    }
    if (!view.write_memory(0, memory)) {
      return HostResult::trap(result.gas_consumed);
    }
    return HostResult::continue_with(static_cast<uint32_t>(result.result_code),
                                     result.gas_consumed);
  case std::to_underlying(HostFnErrorCode::Return):
    // Return and Exit terminate the VM invocation cleanly after committing the
    // same guest-memory output writes the WASM path would expose.
    if (!view.write_memory(0, memory)) {
      return HostResult::trap(result.gas_consumed);
    }
    return HostResult::halt(0, result.gas_consumed);
  case std::to_underlying(HostFnErrorCode::Exit):
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
