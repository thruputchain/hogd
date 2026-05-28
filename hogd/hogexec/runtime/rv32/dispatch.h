#pragma once

#include <cstdint>
#include <span>

#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

// Artifact encoding is 16 bytes per dispatch entry:
// method_id:u64, pc:u32, reserved:u32. The in-memory struct stores only the
// meaningful fields; reserved bytes must parse as zero.
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

[[nodiscard]]
bool dispatch_table_is_valid(std::span<const DispatchEntry> dispatch_table,
                             Program const &program);

} // namespace hogd::rv32
