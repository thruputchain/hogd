#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetDeployAddress_inner(SystemCallHandler &syscall_handler,
                               const SHA256Hash &input_nonce,
                               const SHA256Hash &input_contract_hash,
                               ContractAddress &output_to_be_deployed)
{
  output_to_be_deployed =
      syscall_handler.get_deploy_address(input_nonce, input_contract_hash);

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_DeployContract_inner(SystemCallHandler &syscall_handler,
                             const SHA256Hash &input_nonce,
                             const SHA256Hash &input_contract_hash,
                             ContractAddress &output_deployed_address)
{
  auto status =
      syscall_handler.deploy_contract(input_nonce, input_contract_hash);
  if (!status) {
    return {status.error().code(), 0};
  }
  output_deployed_address = *status;

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
