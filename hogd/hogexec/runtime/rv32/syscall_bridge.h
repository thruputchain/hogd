#pragma once

#include <optional>

#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/host.h"

namespace hogd::rv32 {

// Register-native bridge ABI: the guest places the syscall number in a0 and up
// to seven argument words in a1..a7, then executes ecall. Each RV32 register is
// sign-extended to the uint64_t that syscall_entry consumes. Sign- (not zero-)
// extension is required so a guest can express signed scalars and the shared
// parser's UINT64_MAX optional-pointer null sentinel (0xFFFFFFFF -> all-ones);
// valid guest pointers and lengths keep bit 31 clear, so for them sign- and
// zero-extension coincide.
//
// ABI constraint on syscall signatures: every register-passed argument must be
// representable as a sign-extended 32-bit value -- a guest pointer/length, a
// signed 32-bit scalar, or an unsigned scalar < 2^31. A scalar needing the full
// 64-bit range (or the full unsigned-32 range) must travel through guest memory
// by pointer instead. wrap_outer's static_assert enforces the <=7-word count;
// the <=32-bit-scalar half is currently a documented convention, not enforced.
//
// The syscall ABI version is validated at artifact load time, so no per-call
// version register is needed. Results: the syscall return code is delivered in
// a0 and any output buffers/lengths are written into guest memory through the
// pointers the guest supplied; there is no result footer.
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
