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
  // Priced as the equivalent kNonnegativeIntSetAdd, whose exits all pay the
  // access, so this is charged up front and unconditionally. Flat, with no
  // load_gas on top: the mutation stages an add-only item and never reaches
  // the database. Charging before the call also keeps this a plain forwarder
  // rather than a coroutine, so the write costs no frame of its own.
  consume_gas(kStorageAccessCost);
  return handler_.modify_native_token_balance(address, amount);
}

} // namespace hogd
