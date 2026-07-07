#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

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

// Compile-time-width overload for the guest hot path: load/store Width is a
// constant 1/2/4 and address is uint32_t. Widening address to uint64_t makes
// `address + Width` a 64-bit add that cannot overflow for these widths
// regardless of size_t (a 32-bit size_t would otherwise wrap 0xFFFFFFFC+4 to 0
// and pass an out-of-bounds address). The predicate is then a single compare
// (one cmp+branch) instead of the runtime form's two, and on 64-bit platforms
// the widen is a no-op. Equivalent to the 3-arg version for these widths.
template<size_t Width>
[[nodiscard]] constexpr bool
memory_range_in_bounds(size_t memory_size, uint32_t address)
{
  return uint64_t{address} + Width <= memory_size;
}

enum class Trap {
  kNone,
  kInvalidInstruction,
  kBadPc,
  kBadMemory,
  kBreakpoint,
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
  kOutOfGas,
  kTrap,
};

// Host traps carry consensus-visible classification: guest-caused bridge ABI
// faults are deterministic, while genuine host failures remain fatal.
enum class HostTrapKind {
  kDeterministic,
  kFatal,
};

struct HostContinuePayload {
  uint32_t value = 0;
};

struct HostHaltPayload {
  uint32_t code = 0;
};

struct HostOutOfGasPayload {};

struct HostTrapPayload {
  HostTrapKind kind = HostTrapKind::kFatal;
  uint32_t code = 0;
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

// Narrow host-facing view of live machine state. Hosts can inspect registers
// and mutate data memory, but cannot see or modify immutable Program code.
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

// Result of one host ECALL. The status and payload are constructed together so
// callers cannot accidentally interpret a return value as a halt code or trap
// code; gas_consumed is debited before the payload is committed.
struct HostResult {
  static HostResult continue_with(uint32_t value = 0, uint64_t gas_consumed = 0)
  {
    return HostResult(HostStatus::kContinue, gas_consumed,
                      HostContinuePayload{.value = value});
  }
  static HostResult halt(uint32_t code = 0, uint64_t gas_consumed = 0)
  {
    return HostResult(HostStatus::kHalt, gas_consumed,
                      HostHaltPayload{.code = code});
  }
  // Host OutOfGas keeps pc advanced and guest-memory writes persist (no
  // per-ecall rollback), but all engine OOG paths leave gas_remaining at zero;
  // transaction-level abort is the runtime layer's responsibility.
  static HostResult out_of_gas(uint64_t gas_consumed = 0)
  {
    return HostResult(HostStatus::kOutOfGas, gas_consumed,
                      HostOutOfGasPayload{});
  }
  static HostResult trap(uint64_t gas_consumed = 0, uint32_t trap_code = 0)
  {
    return HostResult(HostStatus::kTrap, gas_consumed,
                      HostTrapPayload{.kind = HostTrapKind::kFatal,
                                      .code = trap_code});
  }
  static HostResult deterministic_trap(uint64_t gas_consumed = 0,
                                       uint32_t trap_code = 0)
  {
    return HostResult(HostStatus::kTrap, gas_consumed,
                      HostTrapPayload{.kind = HostTrapKind::kDeterministic,
                                      .code = trap_code});
  }

  [[nodiscard]] uint32_t continue_value() const
  {
    assert(status == HostStatus::kContinue);
    const auto *payload = std::get_if<HostContinuePayload>(&payload_);
    assert(payload != nullptr);
    return payload == nullptr ? 0 : payload->value;
  }

  [[nodiscard]] uint32_t halt_code() const
  {
    assert(status == HostStatus::kHalt);
    const auto *payload = std::get_if<HostHaltPayload>(&payload_);
    assert(payload != nullptr);
    return payload == nullptr ? 0 : payload->code;
  }

  [[nodiscard]] HostTrapPayload trap_payload() const
  {
    assert(status == HostStatus::kTrap);
    const auto *payload = std::get_if<HostTrapPayload>(&payload_);
    assert(payload != nullptr);
    return payload == nullptr ? HostTrapPayload{} : *payload;
  }

  HostStatus status = HostStatus::kContinue;
  uint64_t gas_consumed = 0;

private:
  using Payload = std::variant<HostContinuePayload, HostHaltPayload,
                               HostOutOfGasPayload, HostTrapPayload>;

  HostResult(HostStatus status_value, uint64_t gas, Payload payload)
    : status(status_value), gas_consumed(gas), payload_(payload)
  {}

  Payload payload_ = HostContinuePayload{};
};

class Host {
public:
  virtual ~Host() = default;
  virtual HostResult ecall(MachineView view,
                           const EcallArgs &args) noexcept = 0;
};

} // namespace hogd::rv32
