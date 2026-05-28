#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

class MachineTestPeer;

class Machine {
public:
  Machine(std::shared_ptr<const Program> program, uint32_t memory_bytes,
          uint64_t gas, uint32_t initial_pc = 0);

  void reset(uint64_t gas);

  [[nodiscard]] StepResult step();
  [[nodiscard]] StepResult complete_ecall(const HostResult &result);
  [[nodiscard]] StepResult run(Host &host);

  [[nodiscard]] uint32_t pc() const { return pc_; }
  [[nodiscard]] uint32_t reg(size_t index) const
  {
    return index < registers_.size() ? registers_[index] : 0;
  }
  [[nodiscard]] uint64_t gas_remaining() const { return gas_remaining_; }
  [[nodiscard]] Trap trap() const { return trap_; }
  [[nodiscard]] HostTrapPayload host_trap_payload() const
  {
    assert(trap_ == Trap::kHostTrap);
    assert(host_trap_payload_.has_value());
    return host_trap_payload_.value_or(HostTrapPayload{});
  }
  [[nodiscard]] uint32_t host_trap_code() const
  {
    return host_trap_payload().code;
  }
  [[nodiscard]] HostTrapKind host_trap_kind() const
  {
    return host_trap_payload().kind;
  }
  [[nodiscard]] bool halted() const { return halted_; }
  [[nodiscard]] uint32_t halt_code() const { return halt_code_; }
  [[nodiscard]] std::optional<EcallArgs> pending_ecall() const
  {
    return pending_ecall_;
  }

  [[nodiscard]] std::span<const uint8_t> memory() const { return memory_; }
  [[nodiscard]] MachineView view();

private:
  friend class MachineTestPeer;

  bool consume_gas(uint64_t amount);
  void set_trap(Trap trap);
  void set_host_trap(HostTrapPayload payload);
  void write_reg(uint8_t index, uint32_t value);
  bool validate_control_target(uint32_t target);
  bool check_load(uint32_t address, uint32_t width);
  bool check_store(uint32_t address, uint32_t width);
  uint32_t load(uint32_t address, uint32_t width) const;
  void store(uint32_t address, uint32_t width, uint32_t value);
  StepResult execute(const DecodedInstruction &instruction);

  std::shared_ptr<const Program> program_;
  std::array<uint32_t, kRegisterCount> registers_ = {};
  uint32_t initial_pc_ = 0;
  uint32_t pc_ = 0;
  std::vector<uint8_t> memory_;
  uint64_t gas_remaining_ = 0;
  Trap trap_ = Trap::kNone;
  std::optional<HostTrapPayload> host_trap_payload_;
  Trap construction_trap_ = Trap::kNone;
  bool halted_ = false;
  uint32_t halt_code_ = 0;
  std::optional<EcallArgs> pending_ecall_;
  std::vector<uint8_t> ecall_memory_snapshot_;
};

} // namespace hogd::rv32
