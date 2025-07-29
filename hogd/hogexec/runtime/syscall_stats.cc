#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <cstring>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetTransactionStatistics_inner(SystemCallHandler & /*unused*/,
                                       WrappedGasAmount remaining_gas,
                                       TransactionStatistic input_statistic,
                                       uint64_t &output_result)
{
  switch (input_statistic) {
  case TransactionStatistic::kTxStatGasRemaining:
    output_result = remaining_gas.gas_amount_;
    return {SyscallErrorCode::kNone, 0};
  default:
    return {SyscallErrorCode::kInvalidSyscallInput, 0};
  }
};

} // namespace hogd
