#include "hogd/test_ffi/mock_runtime.h"

#include <cassert>
#include <memory>
#include <string_view>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "x23/cleanup.h"

namespace hogd {

void
MockRuntimeContext::set_trampoline(int32_t contract,
                                   ForeignEntryPoint entry_point,
                                   void *foreign_context)
{
  mocked_contracts_.insert_or_assign(contract, MockContract(entry_point,
                                                            foreign_context));
}

std::pair<InvokeStatus<void>, GasAmount>
MockRuntime::invoke_internal(MethodID method_id, uint64_t gas_limit)
{
  reset_gas(gas_limit);
  auto *context = SyscallHandlerContext::get_context();
  assert(context != nullptr);
  context->push_mock_runtime(this);
  // Pop on every exit path. The foreign callback reached through
  // backing_contract_.invoke() is not noexcept, so a C++ exception unwinding
  // through it must still restore the mock-runtime stack -- otherwise the
  // leaked entry leaves the handler permanently uncloseable (issue #2470).
  x23::Defer pop_runtime([this, context]() {
    auto *runtime = context->pop_mock_runtime();
    assert(runtime == this);
    (void)runtime;
  });
  auto res = backing_contract_.invoke(method_id);
  return {res, gas_consumed()};
}

SystemStatus<std::unique_ptr<Runtime>>
MockRuntimeContext::make_new_runtime(
    DeployedContract const &deployed_contract,
    SystemCallHandler *syscall_handler)
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
  if (deployed_contract_hash.contract_type == ContractType::kMockTest) {

    auto contract =
        syscall_handler->resolve_contract(deployed_contract_hash.hash);
    if (!contract.has_value()) {
      return contract.error();
    }
    if (contract.value() == nullptr) {
      error("Unrecoverable: Contract lookup failed: {}",
            show_byte_array(deployed_contract_hash.hash));
      return SystemError{SystemErrorCode::Unrecoverable};
    }

    if (contract.value()->contract_type() != ContractType::kMockTest) {
      error("Unrecoverable: Attempt made to access a non-mock test contract "
            "from MockRuntimeContext: {}",
            contract.value()->contract_type());
      return SystemError{SystemErrorCode::Unrecoverable};
    }

    int32_t mock_contract = contract.value()->mock_contract_id();
    auto it = mocked_contracts_.find(mock_contract);
    if (it != mocked_contracts_.end()) {
      return std::make_unique<MockRuntime>(it->second);
    }
    return nullptr;
  }
  // otherwise some other contract type; returns whatever the base context
  // wants to do with the contract.
  return base_context_->make_new_runtime(deployed_contract, syscall_handler);
}

} // namespace hogd
