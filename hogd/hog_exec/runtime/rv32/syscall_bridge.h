#pragma once

#include <optional>

#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/host.h"

namespace hogd::rv32 {

// Register-native bridge ABI: the guest places the syscall number in a0 and up
// to seven argument words in a1..a7, then executes ecall. Each RV32 register is
// forwarded as one uint32_t syscall argument slot, matching the shared wasm32
// syscall ABI. The shared parser interprets signed values from their two's
// complement bit pattern, uses UINT32_MAX as the optional-pointer null
// sentinel, and bounds-checks guest pointers and lengths against guest memory.
//
// ABI constraint on syscall signatures: every register-passed argument must be
// representable in one 32-bit slot -- a guest pointer/length, a signed or
// unsigned 32-bit scalar, or an enum. A scalar needing the full 64-bit range
// must travel through guest memory by pointer instead. wrap_outer's
// static_assert enforces the <=7-word count; SyscallArgParams<T> and
// narrow_slot enforce the <=32-bit by-value scalar shape.
//
// The syscall ABI version is validated at artifact load time, so no per-call
// version register is needed. Results: the syscall return code is delivered in
// a0 and any output buffers/lengths are written into guest memory through the
// pointers the guest supplied; there is no result footer. This canonical-test
// bridge stages those writes through scratch memory before committing. The
// production threaded runtime calls syscall_entry with live guest memory
// instead.
class SyscallBridgeHost final : public Host {
public:
  SyscallBridgeHost(ExternHostCallContext context, HostFn8 entry);

  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override;

  [[nodiscard]] std::optional<TrampolineResult> last_result() const
  {
    return last_result_;
  }

private:
  ExternHostCallContext context_ = nullptr;
  HostFn8 entry_ = nullptr;
  std::optional<TrampolineResult> last_result_;
};

} // namespace hogd::rv32
