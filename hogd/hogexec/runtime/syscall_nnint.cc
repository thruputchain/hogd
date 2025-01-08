#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_NonnegativeIntGet_inner(SystemCallHandler &syscall_handler,
                                const StorageKey &input_key,
                                int128_t &output_current_set,
                                int128_t &output_current_add)
{
  auto status = syscall_handler.get_nnint(input_key);
  if (!status) {
    return {status.error().code(), 0};
  }

  output_current_set = status->first;
  output_current_add = status->second;

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_NonnegativeIntSetAdd_inner(SystemCallHandler &syscall_handler,
                                   const StorageKey &input_key,
                                   const int128_t &input_set_value,
                                   const int128_t &input_add_value)
{
  auto status =
      syscall_handler.set_nnint(input_key, input_set_value, input_add_value);
  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
