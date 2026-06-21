#include "hogd/hogexec/runtime/syscall_inner.h"

#include <exception>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetTransactionStatistics_inner(SystemCallHandler &syscall_handler,
                                       WrappedGasAmount remaining_gas,
                                       TransactionStatistic input_statistic,
                                       uint64_t &output_result) noexcept
{
  switch (input_statistic) {
  case TransactionStatistic::GasRemaining:
    output_result = remaining_gas.get();
    if (auto *trace_data = syscall_handler.get_syscall_trace_data<
                           SyscallNo::kGetTransactionStatistics>()) {
      try {
        trace_data->outputs.emplace(
            SyscallGetTransactionStatisticsOutputs{.result = output_result});
      } catch (std::exception &e) {
        report_dropped_syscall_trace(e.what());
        return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
      }
    }
    return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
  default:
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }
};

} // namespace hogd
