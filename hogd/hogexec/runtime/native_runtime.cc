#include "hogd/hogexec/runtime/native_runtime.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "types.hh"

namespace hogd {

InvokeStatus<int32_t>
NativeContract::invoke(MethodID method, SystemCallHandler &syscall_handler)
{
  auto it = functions_.find(method);
  if (it == functions_.end())
    return InvokeError{InvokeErrorCode::kDeterministic};
  return it->second(syscall_handler);
}

std::unique_ptr<Runtime>
NativeContext::make_new_runtime(Contract const &contract,
                                SystemCallHandler *syscall_handler,
                                bool /*unused*/)
{
  if (contract.contract_type() != ContractType::kNative)
    return nullptr;

  auto host_index = contract.native_contract_id();

  auto it = contracts_.find(host_index);
  if (it == contracts_.end())
    return nullptr;

  return std::make_unique<NativeRuntime>(*it->second,
                                         make_new_host_call_context(
                                             syscall_handler));
}

NativeRuntime::NativeRuntime(NativeContract &contract,
                             std::unique_ptr<HostCallContext> host_call_context)
  : Runtime(std::move(host_call_context)), contract_(contract)
{}

InvokeStatus<int32_t>
NativeRuntime::invoke_internal(std::string_view method_name)
{
  auto method_id = Runtime::method_id_from_name(std::string(method_name));
  if (!method_id) {
    return InvokeError{InvokeErrorCode::kUnrecoverable};
  }

  return contract_.invoke(*method_id, *host_call_context_->syscall_handler);
}

} // namespace hogd
