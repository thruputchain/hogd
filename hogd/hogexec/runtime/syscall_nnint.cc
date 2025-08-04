#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cassert>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_NonnegativeIntGet_inner(SystemCallHandler &syscall_handler,
                                const StorageKeyContractData &input_key,
                                bool input_force_load_from_snapshot,
                                int128_t &output_current_set,
                                bool &output_current_set_opt,
                                int128_t &output_current_add)
{
  auto status =
      syscall_handler.get_nnint(input_key, input_force_load_from_snapshot);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }
  if (status.value().value().first.has_value()) {
    output_current_set = *(status.value().value().first);
    output_current_set_opt = true;
  }
  else {
    output_current_set_opt = false;
  }
  output_current_add = status.value().value().second;

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_NonnegativeIntSetAdd_inner(SystemCallHandler &syscall_handler,
                                   const StorageKeyContractData &input_key,
                                   const int128_t *optional_input_set_value,
                                   int128_t input_add_value)
{
  std::optional<int128_t> set_value = std::nullopt;
  if (optional_input_set_value) {
    set_value = *optional_input_set_value;
  }
  auto status =
      syscall_handler.set_nnint(input_key, set_value, input_add_value);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
