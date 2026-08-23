#include "hogd/hog_exec/runtime/syscall_inner.h"

#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"

namespace hogd {

AsyncSyscallInnerResult
syscall_GetTransactionStatistics_inner(SystemCallHandler &syscall_handler,
                                       WrappedGasAmount remaining_gas,
                                       TransactionStatistic input_statistic,
                                       uint64_t &output_result)
{
  switch (input_statistic) {
  case TransactionStatistic::GasRemaining:
    output_result = remaining_gas.get();
    if (auto *trace_data = syscall_handler.get_syscall_trace_data<
                           SyscallNo::kGetTransactionStatistics>()) {
      trace_data->outputs.emplace(
          SyscallGetTransactionStatisticsOutputs{.result = output_result});
    }
    co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                                 WrappedGasAmount{0}};
  default:
    co_return SyscallInnerResult{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
        WrappedGasAmount{0}};
  }
};

} // namespace hogd
