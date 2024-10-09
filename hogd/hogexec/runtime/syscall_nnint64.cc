#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_NonnegativeInt64Get_inner(SystemCallHandler &syscall_handler,
                                  const StorageKey &input_key,
                                  int64_t &output_result)
{
  auto status = syscall_handler.get_nnint64_value(input_key);

  if (!status) {
    return {status.error().code(), 0};
  }

  output_result = *status;
  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_NonnegativeInt64SetAdd_inner(SystemCallHandler &syscall_handler,
                                     const StorageKey &input_key,
                                     const int64_t input_set_value,
                                     const int64_t input_add_value)
{
  auto status =
      syscall_handler.nnint64_set_add(input_key, input_set_value, input_add_value);

  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_NonnegativeInt64Add_inner(SystemCallHandler &syscall_handler,
                                  const StorageKey &input_key,
                                  const int64_t input_add_value)
{
  // TODO(geoff): consider swapping for an add() syscall natively within
  // StorageProxy
  auto base_status = syscall_handler.get_nnint64_base_value(input_key);

  int64_t base_value = 0;

  if (!base_status) {
    if (base_status.error().code() != SyscallErrorCode::kStorageKeyNExist) {
      return {base_status.error().code(), 0};
    }
    // else base_value is defaulted to 0
  }

  auto status = syscall_handler.nnint64_set_add(input_key, base_value, input_add_value);

  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
