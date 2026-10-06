#include "hogd/hog_exec/runtime/rv32/syscall_bridge.h"

#include <exception>
#include <span>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
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

  // The canonical test bridge stages outputs, discarding writes on syscall
  // faults, OOG, and fatal host errors. Production Rv32Runtime passes live
  // guest memory to syscall_entry; this rollback is specific to the test
  // bridge.
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

  // This bridge drives the non-suspending Machine, whose run loop has no way
  // to yield, so it blocks on the syscall result rather than awaiting it.
  SyscallResult result = {};
  try {
    result = entry_(context_, context, args.a0, args.a1, args.a2, args.a3,
                    args.a4, args.a5, args.a6, args.a7)();
  } catch (...) {
    return HostResult::trap();
  }
  last_result_ = result;

  if (result.first.has_value()) {
    // TODO(geoff): reconcile fault-output rollback with the production runtime,
    // where syscall output writes directly to guest memory.
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
    // Discard staged output on OOG.
    return HostResult::out_of_gas(result.second);
  case HostFnErrorCode::Return:
    // Return terminates the VM invocation cleanly after committing the same
    // guest-memory output writes the production runtime would expose.
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
