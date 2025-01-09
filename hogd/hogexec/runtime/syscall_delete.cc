#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_DeleteObject_inner(SystemCallHandler& syscall_handler, const StorageKey& input_key)
{
  auto status =
      syscall_handler.delete_object(input_key);
  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}


} // namespace hogd
