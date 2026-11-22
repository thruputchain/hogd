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
  // One price for any value: the work is bounded by the width of the value,
  // not by what it holds.
  const WrappedGasAmount gas{kU256SqrtCost};
  // Reserve it before doing the arithmetic. Exact exhaustion faults here too.
  if (remaining_gas.get() <= gas.get()) {
    return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
  }
  output_root = limbs_to_u128(limbs_isqrt(limbs_from_le_bytes(input_value)));
  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kU256Sqrt>()) {
    trace_data->outputs.emplace(
        SyscallU256SqrtOutputs{.root = to_xdr(output_root)});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, gas};
}

// The two MulDivRem calls. A zero divisor is refused before anything is
// reserved and bills nothing, as an oversized Ristretto255Msm input is. No
// output has been assigned by then, and the wrapper writes no output of a call
// that returns an error. Any other call has one price, reserved before the
// arithmetic as U256Sqrt's is.

// Experimental API; encoding and pricing may change before stabilization.
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_U256MulDivRem_inner(SystemCallHandler &syscall_handler,
                            WrappedGasAmount remaining_gas,
                            const U256Le &input_a, const U256Le &input_b,
                            const U256Le &input_divisor,
                            U256Le &output_quotient_lo,
                            U256Le &output_quotient_hi,
                            U256Le &output_remainder)
{
  const Limbs<4> divisor = limbs_from_le_bytes(input_divisor);
  if (divisor == Limbs<4>{}) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }
  const WrappedGasAmount gas{kU256MulDivRemCost};
  if (remaining_gas.get() <= gas.get()) {
    return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
  }
  const auto divided = limbs_mul_div_rem(limbs_from_le_bytes(input_a),
                                         limbs_from_le_bytes(input_b), divisor);
  // Not empty: the divisor is not zero.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const LimbsMulDivRem<4> &result = divided.value();
  output_quotient_lo = limbs_to_le_bytes(result.quotient_lo);
  output_quotient_hi = limbs_to_le_bytes(result.quotient_hi);
  output_remainder = limbs_to_le_bytes(result.remainder);
  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kU256MulDivRem>()) {
    trace_data->outputs.emplace(
        SyscallU256MulDivRemOutputs{.quotient_lo = output_quotient_lo,
                                    .quotient_hi = output_quotient_hi,
                                    .remainder = output_remainder});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, gas};
}

// Experimental API; encoding and pricing may change before stabilization.
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_U128MulDivRem_inner(SystemCallHandler &syscall_handler,
                            WrappedGasAmount remaining_gas,
                            const uint128_t &input_a, const uint128_t &input_b,
                            const uint128_t &input_divisor,
                            uint128_t &output_quotient_lo,
                            uint128_t &output_quotient_hi,
                            uint128_t &output_remainder)
{
  if (input_divisor == 0) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }
  const WrappedGasAmount gas{kU128MulDivRemCost};
  if (remaining_gas.get() <= gas.get()) {
    return {HostFnError{HostFnErrorCode::OutOfGas}, remaining_gas};
  }
  const auto divided =
      limbs_mul_div_rem(limbs_from_u128(input_a), limbs_from_u128(input_b),
                        limbs_from_u128(input_divisor));
  // Not empty: the divisor is not zero.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const LimbsMulDivRem<2> &result = divided.value();
  output_quotient_lo = limbs_to_u128(result.quotient_lo);
  output_quotient_hi = limbs_to_u128(result.quotient_hi);
  output_remainder = limbs_to_u128(result.remainder);
  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kU128MulDivRem>()) {
    trace_data->outputs.emplace(
        SyscallU128MulDivRemOutputs{.quotient_lo = to_xdr(output_quotient_lo),
                                    .quotient_hi = to_xdr(output_quotient_hi),
                                    .remainder = to_xdr(output_remainder)});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, gas};
}

} // namespace hogd
