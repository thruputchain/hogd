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
#include "x23/format.h"

namespace hogd {

InvokeStatus<int32_t>
MockContract::invoke(std::string const &method)
{
  MethodID id = 0;
  try {
    id = Runtime::method_id_from_name(method);
  } catch (std::invalid_argument &e) {
    std::printf("name parse failed\n");
    return InvokeError{InvokeErrorCode::kUnrecoverable};
  }
  std::printf("start entry point\n");
  auto res = entry_point_(foreign_context_, id);
  std::printf("done entrypoint\n");

  auto code = static_cast<InvokeErrorCode>(res.panic);

  if (code == InvokeErrorCode::kNone) {
    std::printf("returning kNone\n");
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
  std::printf("start MockRuntime::invoke_internal %s\n", method_name.c_str());
  return backing_contract_.invoke(method_name);
}

std::unique_ptr<Runtime>
MockRuntimeContext::make_new_runtime(SHA256Hash const &hash,
                                     const Contract *contract,
                                     SystemCallHandler *syscall_handler)
{
  auto it = mocked_contracts_.find(hash);
  if (it != mocked_contracts_.end()) {
    std::printf("creating new mock runtime for addr %s\n",
                x23::show(hash).c_str());
    return std::make_unique<MockRuntime>(it->second);
  }
  std::printf("creating base runtime for addr %s\n", x23::show(hash).c_str());
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
