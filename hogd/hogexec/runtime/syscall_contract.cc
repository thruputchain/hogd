#include "hogd/hogexec/runtime/syscall_inner.h"

#include <exception>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_types.h"
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
      trace_data->outputs.emplace(SyscallGetDeployAddressOutputs{
          .to_be_deployed = output_to_be_deployed});
    } catch (std::exception &e) {
      report_dropped_syscall_trace(e.what());
      return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_DeployContract_inner(SystemCallHandler &syscall_handler,
                             const SHA256Hash &input_nonce,
                             const SHA256Hash &input_contract_hash,
                             ContractAddress &output_deployed_address) noexcept
{
  // Direct host gas for the contract-resolution work (HogDB read + XDR decode +
  // eager RV32 translation) performed by deploy_contract(). Set on any path
  // that reaches resolution and reported to the outer syscall trampoline so the
  // work is metered even when the deployed contract is never invoked. (#3122)
  uint64_t load_gas = 0;
  auto status = syscall_handler.deploy_contract(input_nonce,
                                                input_contract_hash, &load_gas);
  if (!status) {
    // SystemError ends execution of the whole block; gas reporting is moot.
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!status.value()) {
    return {status.value().error().code(), WrappedGasAmount{load_gas}};
  }
  output_deployed_address = *status.value();

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kDeployContract>()) {
    try {
      trace_data->outputs.emplace(SyscallDeployContractOutputs{
          .deployed_address = output_deployed_address});
    } catch (std::exception &e) {
      report_dropped_syscall_trace(e.what());
      return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{load_gas}};
}

} // namespace hogd
