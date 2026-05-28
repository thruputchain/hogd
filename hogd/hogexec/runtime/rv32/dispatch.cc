#include "hogd/hogexec/runtime/rv32/dispatch.h"

#include <cstdint>
#include <span>

#include "hogd/hogexec/runtime/rv32/program.h"

namespace hogd::rv32 {

bool
dispatch_table_is_valid(std::span<const DispatchEntry> dispatch_table,
                        Program const &program)
{
  if (dispatch_table.empty()) {
    return false;
  }

  uint64_t previous_method_id = 0;
  bool first = true;
  for (const auto &entry : dispatch_table) {
    if (!program.contains_pc(entry.pc) ||
        (!first && entry.method_id <= previous_method_id)) {
      return false;
    }
    previous_method_id = entry.method_id;
    first = false;
  }
  return true;
}

} // namespace hogd::rv32
