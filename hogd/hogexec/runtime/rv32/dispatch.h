#pragma once

#include <cstdint>

#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

inline constexpr uint32_t kDispatchEntryBytes = 16;
inline constexpr uint32_t kDispatchEntryMethodOffset = 0;
inline constexpr uint32_t kDispatchEntryPcOffset = 8;
inline constexpr uint32_t kDispatchEntryReservedOffset = 12;
inline constexpr uint32_t kMaxDispatchEntries =
    kMaxCodeBytes / kInstructionBytes;
inline constexpr uint32_t kMaxDispatchBytes =
    kMaxDispatchEntries * kDispatchEntryBytes;

struct DispatchEntry {
  uint64_t method_id = 0;
  uint32_t pc = 0;
};

} // namespace hogd::rv32
