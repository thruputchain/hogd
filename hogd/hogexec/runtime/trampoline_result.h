#pragma once

#include <utility>

#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

inline TrampolineResult
to_trampoline_result(HostFnStatus<SyscallErrorCode> status,
                     GasAmount gas_consumed) noexcept
{
  if (status) {
    return {.result_code = std::to_underlying(*status),
            .panic = 0,
            .gas_consumed = gas_consumed};
  }
  return {.result_code = 0,
          .panic = std::to_underlying(status.error().code()),
          .gas_consumed = gas_consumed};
}

} // namespace hogd
