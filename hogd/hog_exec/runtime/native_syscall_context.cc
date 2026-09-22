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
  consume_gas(status->load_gas);
  co_return std::move(status->result);
}

x23::Async<SystemStatus<SyscallStatus<void>>>
NativeSyscallContext::modify_native_token_balance(
    ContractAddress const &address, int128_t amount)
{
  auto status = co_await handler_.modify_native_token_balance(address, amount);
  // Priced as the equivalent kNonnegativeIntSetAdd; a rejected mutation
  // charges nothing, matching that syscall. Flat, with no load_gas on top:
  // this stages an add-only item and never reaches the database.
  if (status.has_value() && status->has_value()) {
    consume_gas(kStorageAccessCost);
  }
  co_return std::move(status);
}

} // namespace hogd
