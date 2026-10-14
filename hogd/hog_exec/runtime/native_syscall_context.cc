#include "hogd/hog_exec/runtime/native_syscall_context.h"

#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/async.h"

#include <utility>

namespace hogd {

x23::Async<SystemStatus<SyscallStatus<int128_t>>>
NativeSyscallContext::native_token_balance(ContractAddress const &address)
{
  // Refuse the read unless the frame can pay its full cost, so an underfunded
  // child cannot force unpriced first-touch database work across fresh
  // addresses.
  const GasAmount charge =
      kStorageAccessCost + handler_.peek_native_token_balance_load_gas(address);
  if (charge > gas_remaining()) {
    // Saturate the meter so the caller's error becomes an OutOfGas fault.
    consume_gas(charge);
    const SyscallStatus<int128_t> refused =
        SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    co_return refused;
  }

  auto status = co_await handler_.get_native_token_balance(address);
  if (!status.has_value()) {
    co_return status.error();
  }
  // Match ordinary storage reads and the sibling write helper: the flat access
  // cost on top of load_gas, which is zero once the view is loaded.
  consume_gas(kStorageAccessCost + status->load_gas);
  co_return std::move(status->result);
}

x23::Async<SystemStatus<SyscallStatus<void>>>
NativeSyscallContext::modify_native_token_balance(
    ContractAddress const &address, int128_t amount)
{
  // Refuse before staging the write so an exhausted frame cannot push balance
  // deltas past its gas budget (priced as kNonnegativeIntSetAdd, no load_gas).
  if (kStorageAccessCost > gas_remaining()) {
    // Saturate the meter so the caller's error becomes an OutOfGas fault.
    consume_gas(kStorageAccessCost);
    const SyscallStatus<void> refused =
        SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    co_return refused;
  }
  consume_gas(kStorageAccessCost);
  co_return co_await handler_.modify_native_token_balance(address, amount);
}

} // namespace hogd
