#include "hogd/hogexec/runtime/native_runtime.h"

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

InvokeStatus<void>
NativeContract::invoke(Runtime::MethodID method,
                       SystemCallHandler &syscall_handler,
                       NativeRuntime &runtime) const
{
  // Note: this doesn't build on gcc 15.1; not clear why, but it works on <=14.3
  // and on gcc main (so presumably it's been fixed for the next release), and
  // on clang 20.  https://godbolt.org/z/PnxhrahxP
  auto it = functions_.find(method);
  if (it == functions_.end())
    return InvokeError{InvokeErrorCode::kInvokeDeterministic};
  return it->second(syscall_handler, runtime);
}

SystemStatus<std::unique_ptr<Runtime>>
NativeContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler, bool /*unused*/)
{
  if (deployed_contract_hash.contract_type != ContractType::kNative) {
    error("Unrecoverable: Attempt made to access a non-native contract from "
          "NativeContext: {}",
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }
  assert(syscall_handler != nullptr);

  auto contract =
      syscall_handler->resolve_contract(deployed_contract_hash.hash);
  if (!contract.has_value()) [[unlikely]] {
    return contract.error();
  }
  if (contract.value() == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed: {}",
          deployed_contract_hash.hash);
    return SystemError(SystemErrorCode::kSystemUnrecoverable);
  }

  if (contract.value()->contract_type() != deployed_contract_hash.contract_type)
      [[unlikely]] {
    error("Unrecoverable: Mismatch in contract type: {} != {}",
          contract.value()->contract_type(),
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }

  auto host_index = contract.value()->native_contract_id();

  auto it = contracts_.find(host_index);
  if (it == contracts_.end()) {
    warn("Attempt made to access a native contract that is not defined: {}",
         host_index);
    return nullptr;
  }

  return std::make_unique<NativeRuntime>(*it->second,
                                         make_new_host_call_context(
                                             syscall_handler));
}

NativeRuntime::NativeRuntime(NativeContract &contract,
                             std::unique_ptr<HostCallContext> host_call_context)
  : Runtime(std::move(host_call_context)), contract_(contract)
{}

InvokeStatus<void>
NativeRuntime::invoke_internal(std::string_view method_name)
{
  auto method_id = Runtime::method_id_from_name(std::string(method_name));
  if (!method_id) {
    return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
  }

  return contract_.invoke(*method_id, *host_call_context_->syscall_handler,
                          *this);
}

} // namespace hogd
