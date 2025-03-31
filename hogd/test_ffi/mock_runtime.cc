#include "hogd/test_ffi/mock_runtime.h"

#include <memory>
#include <string_view>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/test_bindings.h"

namespace hogd {

void
MockRuntimeContext::set_trampoline(uint32_t contract,
                                   ForeignEntryPoint entry_point,
                                   void *foreign_context)
{
  mocked_contracts_.emplace(contract,
                            MockContract(entry_point, foreign_context));
}

InvokeStatus<int32_t>
MockRuntime::invoke_internal(std::string_view method_name)
{
  return backing_contract_.invoke(method_name);
}

std::unique_ptr<Runtime>
MockRuntimeContext::make_new_runtime(Contract const &contract,
                                     SystemCallHandler *syscall_handler,
                                     bool do_contract_verify)
{
  if (contract.contract_type() == ContractType::kMockTest) {
    uint32_t mock_contract = contract.mock_contract_id();
    auto it = mocked_contracts_.find(mock_contract);
    if (it != mocked_contracts_.end()) {
      return std::make_unique<MockRuntime>(it->second);
    }
    return nullptr;
  }
  // otherwise some other contract type; returns whatever the base context
  // wants to do with the contract.
  return base_context_->make_new_runtime(contract, syscall_handler,
                                         do_contract_verify);
}

bool
MockRuntimeContext::link_fn_nargs(std::string_view module_name,
                                  std::string_view fn_name, void *fn_ptr,
                                  uint8_t nargs)
{

  // if linking anything other than syscall_handler, note that
  // MockRuntimeContext handles syscalls via a different mechanism.  Regular
  // WASM Linking not supported

  return base_context_->link_fn_nargs(module_name, fn_name, fn_ptr, nargs);
}

} // namespace hogd
