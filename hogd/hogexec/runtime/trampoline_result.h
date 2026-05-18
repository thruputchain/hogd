#pragma once

#include <utility>

#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "syscall_error.hh"

namespace hogd {

inline TrampolineResult
to_trampoline_result(HostFnStatus<SyscallErrorCode> status) noexcept
{
  if (status) {
    return {.result_code = std::to_underlying(status.value()), .panic = 0};
  }
  return {.result_code = 0, .panic = std::to_underlying(status.error().code())};
}

} // namespace hogd
