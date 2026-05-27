#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/host.h"

namespace hogd::rv32 {

inline constexpr uint32_t kSyscallBridgeVersion = 0;
inline constexpr size_t kSyscallFrameArgWords = 11;
inline constexpr size_t kSyscallFrameResultOffset =
    kSyscallFrameArgWords * sizeof(uint64_t);
inline constexpr size_t kSyscallFrameResultBytes = 16;
inline constexpr size_t kSyscallFrameBytes =
    kSyscallFrameResultOffset + kSyscallFrameResultBytes;
static_assert(sizeof(TrampolineResult) == kSyscallFrameResultBytes);
static_assert(kSyscallFrameBytes == 104);

class SyscallBridgeHost final : public Host {
public:
  SyscallBridgeHost(ExternHostCallContext context, HostFn11 entry);

  HostResult ecall(MachineView view, const EcallArgs &args) noexcept override;

  [[nodiscard]] std::optional<TrampolineResult> last_result() const
  {
    return last_result_;
  }

private:
  ExternHostCallContext context_ = nullptr;
  HostFn11 entry_ = nullptr;
  std::optional<TrampolineResult> last_result_;
};

} // namespace hogd::rv32
