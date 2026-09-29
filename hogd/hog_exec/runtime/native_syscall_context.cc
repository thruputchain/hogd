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
  // Prove the frame can pay for a cold database load before performing one.
  // Otherwise a child invoked with only a couple of gas could force an unpriced
  // first-touch balance read -- each retained in the transaction/block caches
  // -- and repeat it across fresh addresses to amplify database work.
  const GasAmount load_gas =
      handler_.peek_native_token_balance_load_gas(address);
  if (load_gas > gas_remaining()) {
    consume_gas(load_gas); // saturate the meter; the frame faults OutOfGas
    // A syscall-level error short-circuits the caller, which then reports a
    // recoverable frame fault; the saturated meter makes invoke_impl upgrade
    // that to OutOfGas.
    const SyscallStatus<int128_t> refused =
        SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    co_return refused;
  }

  auto status = co_await handler_.get_native_token_balance(address);
  if (!status.has_value()) {
    co_return status.error();
  }
  // The read did no more load work than the peek priced, and that was proven
  // affordable above, so this only saturates the meter when it lands exactly at
  // the limit -- which invoke_impl already reports as OutOfGas.
  consume_gas(status->load_gas);
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
