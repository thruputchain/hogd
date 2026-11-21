#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/wide_math.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

// Experimental API; encoding and pricing may change before stabilization.
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_U256Sqrt_inner(SystemCallHandler &syscall_handler,
                       WrappedGasAmount remaining_gas,
                       const U256Le &input_value, uint128_t &output_root)
{
  // One price for any value: the root takes the same 128 steps whatever it is.
  const WrappedGasAmount gas{kU256SqrtCost};
  // Reserve it before doing the arithmetic. Exact exhaustion faults here too.
  if (remaining_gas.get() <= gas.get()) {
    return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
  }
  output_root = wide_math::to_u128(
      wide_math::isqrt<2>(wide_math::from_le_bytes(input_value)));
  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kU256Sqrt>()) {
    trace_data->outputs.emplace(
        SyscallU256SqrtOutputs{.root = to_xdr(output_root)});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, gas};
}

} // namespace hogd
