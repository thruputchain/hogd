#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "types.hh"
#include "x23/async.h"

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

x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
NativeContract::invoke(Runtime::MethodID method,
                       SystemCallHandler &syscall_handler,
                       uint64_t gas_limit) const
{
  using result_t = std::pair<InvokeStatus<void>, GasAmount>;
  const auto *fn = find_function(method);
  if (fn == nullptr) {
    co_return result_t{InvokeError{InvokeErrorCode::Deterministic}, 0};
  }
  co_return co_await invoke_impl(*fn, syscall_handler, gas_limit);
}

x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
NativeContract::invoke_impl(function_t const &fn,
                            SystemCallHandler &syscall_handler,
                            uint64_t gas_limit)
{
  // ctx lives in this frame: the body suspends while holding a reference to it.
  NativeSyscallContext ctx(syscall_handler, gas_limit);
  auto status = co_await fn(ctx);

  GasAmount gas_consumed = ctx.total_gas_consumed();
  CHECK(gas_consumed <= gas_limit);

  if (!status.has_value()) {
    switch (status.error().code()) {
    case InvokeErrorCode::Unrecoverable:
    case InvokeErrorCode::ValidationFailure:
    case InvokeErrorCode::Abandon:
    case InvokeErrorCode::OutOfGas:
      co_return std::pair{status, gas_consumed};
    case InvokeErrorCode::None:
    case InvokeErrorCode::Deterministic:
      break;
    }
  }

  // These are the only error codes that can get upgraded to OutOfGas
  if (gas_limit == gas_consumed) {
    co_return std::pair{InvokeStatus<void>{
                            InvokeError{InvokeErrorCode::OutOfGas}},
                        gas_consumed};
  }
  co_return std::pair{status, gas_consumed};
}

x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
NativeContext::make_new_runtime(DeployedContract const &deployed_contract,
                                SystemCallHandler *syscall_handler)
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
  if (deployed_contract_hash.contract_type != ContractType::kNative) {
    error("Unrecoverable: Attempt made to access a non-native contract from "
          "NativeContext: {}",
          deployed_contract_hash.contract_type);
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(syscall_handler != nullptr);

  auto contract =
      co_await syscall_handler->resolve_contract(deployed_contract_hash.hash);
  if (!contract.has_value()) [[unlikely]] {
    co_return contract.error();
  }
  if (contract.value() == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed: {}",
          show_byte_array(deployed_contract_hash.hash));
    co_return SystemError(SystemErrorCode::Unrecoverable);
  }

  if (contract.value()->contract_type() != deployed_contract_hash.contract_type)
      [[unlikely]] {
    error("Unrecoverable: Mismatch in contract type: {} != {}",
          contract.value()->contract_type(),
          deployed_contract_hash.contract_type);
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }

  auto host_index = contract.value()->native_contract_id();

  auto it = contracts_.find(std::to_underlying(host_index));
  if (it == contracts_.end()) {
    warn("Attempt made to access a native contract that is not defined: {}",
         host_index);
    co_return nullptr;
  }

  co_return std::make_unique<NativeRuntime>(*it->second, syscall_handler);
}

NativeRuntime::NativeRuntime(NativeContract &contract,
                             SystemCallHandler *syscall_handler)
  : Runtime(syscall_handler), contract_(contract)
{}

x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
NativeRuntime::invoke_internal(MethodID method_id, uint64_t gas_limit)
{
  return contract_.invoke(method_id, *syscall_handler_, gas_limit);
}

} // namespace hogd
