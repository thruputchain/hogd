#include "hogd/hogexec/runtime/wasm3_runtime.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/wasm3_wrapper.h"

namespace hogd {

[[nodiscard]] uint64_t
Wasm3Runtime::get_available_gas() const noexcept
{
  return available_gas_;
}

void
Wasm3Runtime::set_available_gas(uint64_t gas) noexcept
{
  available_gas_ = gas;
}

std::unique_ptr<Runtime>
Wasm3Context::make_new_runtime(Contract const &contract,
                               SystemCallHandler *syscall_handler,
                               bool /*unused*/)
{
  if (contract.contract_type() != ContractType::kWasmi)
    return nullptr;

  auto module = wasm3_env_.parse_module(contract.wasmi_contract().data(),
                                        contract.wasmi_contract().size());
  if (!module) {
    return nullptr;
  }

  std::unique_ptr<HostCallContext> host_call_context =
      std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = syscall_handler;

  constexpr static uint32_t kMaxStackBytes = 65536;
  auto runtime =
      wasm3_env_.new_runtime(kMaxStackBytes, host_call_context.get());
  if (!runtime->load(module)) {
    return nullptr;
  }
  // if runtime successfully loads module,
  // it takes a reference to module (a shared_ptr).

  for (auto const &linked_fn : functions_to_link_) {
    if (!module->link_nargs(linked_fn.module_name_.c_str(),
                            linked_fn.fn_name_.c_str(), linked_fn.fn_pointer_,
                            linked_fn.nargs_)) {
      return nullptr;
    }
  }

  // runtime owns module after loading
  return std::make_unique<Wasm3Runtime>(std::move(host_call_context),
                                        std::move(runtime));
}

std::span<std::byte>
Wasm3Runtime::get_memory() noexcept
{
  assert(wasm3_runtime_);
  return wasm3_runtime_->get_memory();
}

InvokeStatus<void>
Wasm3Runtime::invoke_internal(std::string_view method_name)
{
  // input to find_function has to be null-terminated c-string
  std::string method_name_string(method_name);
  auto fn = wasm3_runtime_->find_function(method_name_string.c_str());
  if (!fn) {
    return InvokeError{InvokeErrorCode::kInvokeDeterministic};
  }
  return fn->call();
}

} // namespace hogd
