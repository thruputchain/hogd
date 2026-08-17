#include "hogd/hog_exec/runtime/rv32/syscall_bridge.h"

#include <span>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "syscall_error.hh"

namespace hogd::rv32 {

SyscallBridgeHost::SyscallBridgeHost(ExternHostCallContext context,
                                     SyscallFunction entry)
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
  // syscall_entry mutates WasmContext.memory in place. This canonical-test
  // bridge runs it against scratch memory so faults/OOG can be observed without
  // committing partial writes to Machine memory. The production threaded
  // runtime intentionally skips this copy and exposes live guest memory to
  // syscall_entry.
  const std::span<const uint8_t> view_memory = view.memory();
  std::vector<uint8_t> scratch;
  try {
    scratch.assign(view_memory.begin(), view_memory.end());
  } catch (...) {
    return HostResult::trap();
  }
  const std::span<uint8_t> memory(scratch.data(), scratch.size());
  ExecutionContext context{
      .memory = std::as_writable_bytes(memory),
      .gas_remaining = view.gas_remaining(),
  };

  std::pair<HostFnStatus<SyscallErrorCode>, GasAmount> result = {};
  try {
    result = entry_(context_, context, args.a0, args.a1, args.a2, args.a3,
                    args.a4, args.a5, args.a6, args.a7);
  } catch (...) {
    return HostResult::trap();
  }
  last_result_ = result;

  if (result.first.has_value()) {
    // TODO(geoff): why does this bridge handle things differently from the
    // threaded implementation? Stems from legacy choices on the initial
    // implementation, which we can probably remove.
    if (*result.first == SyscallErrorCode::kSyscallErrorCodeFault) {
      return HostResult::continue_with(std::to_underlying(*result.first),
                                       result.second);
    }

    if (!view.write_memory(0, memory)) {
      return HostResult::trap(result.second);
    }

    return HostResult::continue_with(std::to_underlying(*result.first),
                                     result.second);
  }

  switch (result.first.error().code()) {
  case HostFnErrorCode::NoneOrRecoverable:
    std::terminate();
  case HostFnErrorCode::OutOfGas:
    // The bridge returns out_of_gas before writing any guest memory, so there
    // is nothing to roll back; complete_ecall no longer snapshots memory.
    return HostResult::out_of_gas(result.second);
  case HostFnErrorCode::Return:
    // Return terminates the VM invocation cleanly after committing the same
    // guest-memory output writes the WASM path would expose.
    if (!view.write_memory(0, memory)) {
      return HostResult::trap(result.second);
    }
    return HostResult::halt(0, result.second);

  case HostFnErrorCode::Exit:
    // Exit is a deterministic frame abort on every runtime; the trampoline
    // result_code is not an exit status. Memory still commits, matching the
    // live-memory production bridge.
    if (!view.write_memory(0, memory)) {
      return HostResult::trap(result.second);
    }
    return HostResult::deterministic_trap(result.second);
  case HostFnErrorCode::ValidationFailure:
  case HostFnErrorCode::Abandon:
  case HostFnErrorCode::Unrecoverable:
    // Unknown or fatal host panics trap without committing scratch memory.
    return HostResult::trap(result.second,
                            static_cast<uint32_t>(result.first.error().code()));
  }
  std::terminate();
}

} // namespace hogd::rv32
