#include "hogd/hogexec/runtime/syscall_inner.h"

#include <exception>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetDeployAddress_inner(
    SystemCallHandler &syscall_handler,
    const ContractAddress *optional_input_caller_address,
    const SHA256Hash &input_nonce, const SHA256Hash &input_contract_hash,
    ContractAddress &output_to_be_deployed) noexcept
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
    try {
      trace_data->success().nonce = input_nonce;
      trace_data->success().contract_hash = input_contract_hash;
      trace_data->success().to_be_deployed = output_to_be_deployed;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetDeployAddress,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_DeployContract_inner(SystemCallHandler &syscall_handler,
                             const SHA256Hash &input_nonce,
                             const SHA256Hash &input_contract_hash,
                             ContractAddress &output_deployed_address) noexcept
{
  auto status =
      syscall_handler.deploy_contract(input_nonce, input_contract_hash);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }
  output_deployed_address = *status.value();

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kDeployContract>()) {
    try {
      trace_data->success().nonce = input_nonce;
      trace_data->success().contract_hash = input_contract_hash;
      trace_data->success().deployed_address = output_deployed_address;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kDeployContract,
            e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
