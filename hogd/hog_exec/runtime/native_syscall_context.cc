#include "hogd/hog_exec/runtime/native_syscall_context.h"

#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<SystemStatus<SyscallStatus<int128_t>>>
NativeSyscallContext::native_token_balance(ContractAddress const &address)
{
  auto status = co_await handler_.get_native_token_balance(address);
  if (!status.has_value()) {
    co_return status.error();
  }
  consume_gas(status->load_gas);
  co_return std::move(status->result);
}

} // namespace hogd
