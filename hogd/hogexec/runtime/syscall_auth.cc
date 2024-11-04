#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetAuthPayloadHash_inner(SystemCallHandler &syscall_handler,
                                 const SHA256Hash *optional_input_message,
                                 SHA256Hash &output_auth_payload_hash)
{
  throw std::runtime_error("unimplemented");
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_PushAuthContext_inner(SystemCallHandler &syscall_handler,
                              std::span<const std::byte> input_context)
{
  syscall_handler.push_auth_context(input_context);

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_RequireAuth_inner(SystemCallHandler &syscall_handler,
                          const ContractAddress &input_authorizing_address,
                          std::span<const std::byte> input_action)
{
  throw std::runtime_error("unimplemented");
}

} // namespace hogd
