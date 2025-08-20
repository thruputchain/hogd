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
#include "types.hh"

namespace hogd {

InvokeStatus<void>
NativeContract::invoke(MethodID method, SystemCallHandler &syscall_handler,
                       NativeRuntime &runtime)
{
  // Note: this doesn't build on gcc 15.1; not clear why, but it works on <=14.3
  // and on gcc main (so presumably it's been fixed for the next release), and
  // on clang 20.  https://godbolt.org/z/PnxhrahxP
  auto it = functions_.find(method);
  if (it == functions_.end())
    return InvokeError{InvokeErrorCode::kInvokeDeterministic};
  return it->second(syscall_handler, runtime);
}

std::unique_ptr<Runtime>
NativeContext::make_new_runtime(Contract const &contract,
                                SHA256Hash const & /*unused*/,
                                SystemCallHandler *syscall_handler,
                                bool /*unused*/)
{
  if (contract.contract_type() != ContractType::kNative) {
    warn("Attempt made to access a non-native contract from NativeContext: {}",
         contract.contract_type());
    return nullptr;
  }

  auto host_index = contract.native_contract_id();

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
