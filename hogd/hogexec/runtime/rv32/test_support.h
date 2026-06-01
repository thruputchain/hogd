#pragma once

#include <cstddef>
#include <cstdint>

#include "hogd/hogexec/runtime/rv32/machine.h"

namespace hogd::rv32 {

class MachineTestPeer {
public:
  static void set_pc(Machine &machine, uint32_t pc) { machine.pc_ = pc; }

  static void set_reg(Machine &machine, size_t index, uint32_t value)
  {
    if (index != 0 && index < machine.registers_.size()) {
      machine.registers_[index] = value;
    }
  }
};

} // namespace hogd::rv32
