#include "hogd/hog_exec/runtime/native_syscall_context.h"

#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "types.hh"
#include "x23/async.h"

#include <utility>

namespace hogd {

x23::Async<SystemStatus<SyscallStatus<int128_t>>>
NativeSyscallContext::native_token_balance(ContractAddress const &address)
{
  auto status = co_await handler_.get_native_token_balance(address);
  if (!status.has_value()) {
    co_return status.error();
  }
  // Match ordinary storage reads and the sibling write helper: charge the flat
  // access cost on top of load_gas, which is zero once the view is loaded.
  consume_gas(kStorageAccessCost + status->load_gas);
  co_return std::move(status->result);
}

x23::Async<SystemStatus<SyscallStatus<void>>>
NativeSyscallContext::modify_native_token_balance(
    ContractAddress const &address, int128_t amount)
{
  // Priced as the equivalent kNonnegativeIntSetAdd, charged up front so this
  // stays a plain forwarder. No load_gas: the write never reaches the database.
  consume_gas(kStorageAccessCost);
  return handler_.modify_native_token_balance(address, amount);
}

} // namespace hogd
