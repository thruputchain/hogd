#include "hogd/hogexec/runtime/mock_runtime.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/mock_contract.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/test_ffi/test_bindings.h"
#include "types.hh"

namespace hogd {

InvokeStatus<int32_t>
MockContract::invoke(std::string const &method)
{
  MethodID id = 0;
  try {
    id = Runtime::method_id_from_name(method);
  } catch (std::invalid_argument &e) {
    return InvokeError{InvokeErrorCode::kUnrecoverable};
  }
  auto res = entry_point_(foreign_context_, id);

  auto code = static_cast<InvokeErrorCode>(res.panic);

  if (code == InvokeErrorCode::kNone) {
    return InvokeStatus<int32_t>{res.result_code};
  }
  return InvokeError{code};
}

void
MockRuntimeContext::set_trampoline(SHA256Hash const &contract,
                                   ForeignEntryPoint entry_point,
                                   void *foreign_context)
{
  mocked_contracts_.emplace(contract,
                            MockContract(entry_point, foreign_context));
}

InvokeStatus<int32_t>
MockRuntime::invoke_internal(std::string const &method_name)
{
  return backing_contract_.invoke(method_name);
}

std::unique_ptr<Runtime>
MockRuntimeContext::make_new_runtime(SHA256Hash const &hash,
                                     const Contract *contract,
                                     SystemCallHandler *syscall_handler)
{
  auto it = mocked_contracts_.find(hash);
  if (it != mocked_contracts_.end()) {
    return std::make_unique<MockRuntime>(it->second);
  }
  return base_context_->make_new_runtime(hash, contract, syscall_handler);
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
