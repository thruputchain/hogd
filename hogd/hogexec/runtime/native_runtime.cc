#include "hogd/hogexec/runtime/native_runtime.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

const NativeContract::function_t *
NativeContract::find_function(Runtime::MethodID method) const
{
  auto it = std::ranges::lower_bound(functions_, method, std::ranges::less{},
                                     &function_entry_t::first);
  if (it == functions_.end() || it->first != method) {
    return nullptr;
  }
  return &it->second;
}

std::pair<InvokeStatus<void>, GasAmount>
NativeContract::invoke(Runtime::MethodID method,
                       SystemCallHandler &syscall_handler,
                       uint64_t gas_limit) const
{
  const auto *fn = find_function(method);
  if (fn == nullptr)
    return {InvokeError{InvokeErrorCode::Deterministic}, 0};
  NativeSyscallContext ctx(syscall_handler, gas_limit);
  auto status = (*fn)(ctx);


  GasAmount gas_consumed = ctx.total_gas_consumed();
  CHECK(gas_consumed <= gas_limit);

  if (!status.has_value()) {
    switch (status.error().code())
    {
      case InvokeErrorCode::Unrecoverable:
      case InvokeErrorCode::ValidationFailure:
      case InvokeErrorCode::Abandon:
      case InvokeErrorCode::OutOfGas:
        return {status, gas_consumed};
      case InvokeErrorCode::None:
      case InvokeErrorCode::Deterministic:
        // nothing to do
    }
  }

  // These are the only error codes that can get upgraded to OutOfGas
  if (gas_limit == gas_consumed) {
    return {InvokeError{InvokeErrorCode::OutOfGas}, gas_consumed};
  }
  return {status, gas_consumed};
}

SystemStatus<std::unique_ptr<Runtime>>
NativeContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler)
{
  if (deployed_contract_hash.contract_type != ContractType::kNative) {
    error("Unrecoverable: Attempt made to access a non-native contract from "
          "NativeContext: {}",
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(syscall_handler != nullptr);

  auto contract =
      syscall_handler->resolve_contract(deployed_contract_hash.hash);
  if (!contract.has_value()) [[unlikely]] {
    return contract.error();
  }
  if (contract.value() == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed: {}",
          show_byte_array(deployed_contract_hash.hash));
    return SystemError(SystemErrorCode::Unrecoverable);
  }

  if (contract.value()->contract_type() != deployed_contract_hash.contract_type)
      [[unlikely]] {
    error("Unrecoverable: Mismatch in contract type: {} != {}",
          contract.value()->contract_type(),
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::Unrecoverable};
  }

  auto host_index = contract.value()->native_contract_id();

  auto it = contracts_.find(std::to_underlying(host_index));
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

std::pair<InvokeStatus<void>, GasAmount>
NativeRuntime::invoke_internal(std::string_view method_name, uint64_t gas_limit)
{
  auto method_id = Runtime::method_id_from_name(method_name);
  if (!method_id) {
    error("Unrecoverable: NativeRuntime::invoke_internal failed to parse "
          "method name '{}'",
          method_name);
    return {InvokeError{InvokeErrorCode::Unrecoverable}, 0};
  }

  return contract_.invoke(*method_id, *host_call_context_->syscall_handler,
                          gas_limit);
}

} // namespace hogd
