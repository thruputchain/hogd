#pragma once

#include <cstdint>
#include <optional>

#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/host.h"

namespace hogd::rv32 {

// Canonical test-machine adapter for the shared syscall ABI: a0 selects the
// syscall; a1..a7 carry up to seven 32-bit arguments. Results return in a0 and
// guest output buffers. Full-width 64-bit values must be passed by pointer.
// wrap_outer and SyscallArgParams enforce argument count and scalar width;
// the shared parser checks guest pointer ranges and handles UINT32_MAX nulls.
//
// This bridge stages output in scratch memory. Production Rv32Runtime instead
// calls syscall_entry with live guest memory; see syscall_bridge.cc for the
// test bridge's commit rules.
class SyscallBridgeHost final : public Host {
public:
  // `syscall_api_minor` is the minor the guest was built against; see
  // ExecutionContext.
  SyscallBridgeHost(ExternHostCallContext context, SyscallFunction entry,
                    uint32_t syscall_api_minor = 0);

  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override;

  [[nodiscard]] std::optional<SyscallResult> last_result() const
  {
    return last_result_;
  }

private:
  ExternHostCallContext context_ = nullptr;
  SyscallFunction entry_ = nullptr;
  uint32_t syscall_api_minor_ = 0;
  std::optional<SyscallResult> last_result_;
};

} // namespace hogd::rv32
