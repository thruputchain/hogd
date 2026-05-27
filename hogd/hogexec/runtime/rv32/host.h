#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

[[nodiscard]] constexpr bool
memory_range_in_bounds(size_t memory_size, uint32_t address, size_t width)
{
  // Single bounds predicate for guest and host views. The subtraction form
  // avoids address+width overflow while allowing zero-length end pointers.
  const size_t addr = address;
  return addr <= memory_size && width <= memory_size - addr;
}

enum class Trap {
  kNone,
  kInvalidInstruction,
  kBadPc,
  kBadMemory,
  kMisalignedAccess,
  kOutOfGas,
  kHostTrap,
};

enum class StepResult {
  kContinue,
  kEcall,
  kHalted,
  kTrap,
};

enum class HostStatus {
  kContinue,
  kHalt,
  kTrap,
};

struct EcallArgs {
  uint32_t a0 = 0;
  uint32_t a1 = 0;
  uint32_t a2 = 0;
  uint32_t a3 = 0;
  uint32_t a4 = 0;
  uint32_t a5 = 0;
  uint32_t a6 = 0;
  uint32_t a7 = 0;
};

class MachineView {
public:
  MachineView(uint32_t pc, uint64_t gas_remaining,
              std::span<const uint32_t, kRegisterCount> registers,
              std::span<uint8_t> memory)
    : pc_(pc), gas_remaining_(gas_remaining), registers_(registers),
      memory_(memory)
  {}

  [[nodiscard]] uint32_t pc() const { return pc_; }
  [[nodiscard]] uint64_t gas_remaining() const { return gas_remaining_; }
  [[nodiscard]] uint32_t reg(size_t index) const
  {
    return index < registers_.size() ? registers_[index] : 0;
  }
  [[nodiscard]] std::span<const uint8_t> memory() const { return memory_; }

  [[nodiscard]] bool read_memory(uint32_t address, std::span<uint8_t> out) const
  {
    if (!memory_range_in_bounds(memory_.size(), address, out.size())) {
      return false;
    }
    std::copy_n(memory_.begin() + address, out.size(), out.begin());
    return true;
  }

  [[nodiscard]] bool write_memory(uint32_t address,
                                  std::span<const uint8_t> in) const
  {
    // Host calls may mutate only guest data memory through this view. Code is
    // held in Program, not in the writable memory span.
    if (!memory_range_in_bounds(memory_.size(), address, in.size())) {
      return false;
    }
    std::copy_n(in.begin(), in.size(), memory_.begin() + address);
    return true;
  }

private:
  uint32_t pc_ = 0;
  uint64_t gas_remaining_ = 0;
  std::span<const uint32_t, kRegisterCount> registers_;
  std::span<uint8_t> memory_;
};

struct HostResult {
  static HostResult continue_with(uint32_t value = 0)
  {
    return HostResult{.status = HostStatus::kContinue, .value = value};
  }
  static HostResult halt(uint32_t code = 0)
  {
    return HostResult{.status = HostStatus::kHalt, .value = code};
  }
  static HostResult trap()
  {
    return HostResult{.status = HostStatus::kTrap, .value = 0};
  }

  HostStatus status = HostStatus::kContinue;
  uint32_t value = 0;
};

class Host {
public:
  virtual ~Host() = default;
  virtual HostResult ecall(MachineView view,
                           const EcallArgs &args) noexcept = 0;
};

} // namespace hogd::rv32
