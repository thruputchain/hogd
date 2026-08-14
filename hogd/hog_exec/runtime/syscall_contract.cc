#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <exception>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
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
  // deploy_contract() loads/warms the target contract and records both the
  // deployment and the per-transaction contract-load accounting; those
  // unordered_map/set inserts can throw std::bad_alloc under memory pressure.
  // This syscall body is noexcept, so the exception must not unwind across the
  // boundary and terminate the executor -- translate it into an unrecoverable
  // system error instead (mirrors the resolve_runtime handling in invoke(),
  // issue #3364).
  auto status = [&]() -> SystemStatus<SyscallStatus<ContractAddress>> {
    try {
      return syscall_handler.deploy_contract(input_nonce, input_contract_hash);
    } catch (std::exception const &e) {
      report_dropped_log(e.what());
      return SystemError{SystemErrorCode::Unrecoverable};
    } catch (...) {
      report_dropped_log("unknown exception");
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  }();
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &deploy_status = *status;
  if (!deploy_status) {
    return {deploy_status.error().code(), WrappedGasAmount{0}};
  }
  output_deployed_address = *deploy_status;

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

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
