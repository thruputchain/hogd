#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <utility>

#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetDeployAddress_inner(
    SystemCallHandler &syscall_handler,
    const ContractAddress *optional_input_caller_address,
    const SHA256Hash &input_nonce, const SHA256Hash &input_contract_hash,
    ContractAddress &output_to_be_deployed)
{
  if (optional_input_caller_address) {
    output_to_be_deployed = SystemCallHandler::static_get_deploy_address(
        *optional_input_caller_address, input_nonce, input_contract_hash);
  }
  else {
    output_to_be_deployed = SystemCallHandler::static_get_deploy_address(
        syscall_handler.get_current_address(), input_nonce,
        input_contract_hash);
  }

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetDeployAddress>()) {
    trace_data->outputs.emplace(SyscallGetDeployAddressOutputs{
        .to_be_deployed = output_to_be_deployed});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

MaybeAsyncSyscallInnerResult
syscall_DeployContract_inner(SystemCallHandler &syscall_handler,
                             const SHA256Hash &input_nonce,
                             const SHA256Hash &input_contract_hash,
                             ContractAddress &output_deployed_address)
{
  auto status = co_await syscall_handler.deploy_contract(input_nonce,
                                                         input_contract_hash);
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  auto &deploy_status = *status;
  if (!deploy_status) {
    co_return SyscallInnerResult{deploy_status.error().code(),
                                 WrappedGasAmount{0}};
  }
  output_deployed_address = *deploy_status;

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kDeployContract>()) {
    trace_data->outputs.emplace(SyscallDeployContractOutputs{
        .deployed_address = output_deployed_address});
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
}

} // namespace hogd
