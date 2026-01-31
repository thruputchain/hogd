#pragma once

#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "syscall_error.hh"

namespace hogd {

struct TrampolineResult {
  int32_t result;
  int32_t panic;
};

inline TrampolineResult
to_trampoline_result(HostFnStatus<SyscallErrorCode> status)
{
  if (status) {
    return {.result = std::to_underlying(status.value()), .panic = 0};
  }
  return {.result = 0, .panic = std::to_underlying(status.error().code())};
}

} // namespace hogd
