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
  const auto db_gas_before = handler_.db_read_gas();
  auto result = co_await handler_.get_native_token_balance(address);
  consume_gas(handler_.db_read_gas() - db_gas_before);
  co_return result;
}

} // namespace hogd
