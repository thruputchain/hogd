#include <cassert>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "hogd/hogexec/wasmi/bindings.h"

namespace hogd {

WasmiContext::WasmiContext() : wasmi_context_ptr_(new_wasmi_context())
{}

WasmiContext::~WasmiContext()
{
  free_wasmi_context(wasmi_context_ptr_);
}

std::unique_ptr<Runtime>
WasmiContext::make_new_runtime(Contract const &contract,
                               SystemCallHandler *syscall_handler,
                               bool do_contract_verify)
{
  if (do_contract_verify && !verify_contract(contract)) {
    return {};
  }

  std::unique_ptr<HostCallContext> host_call_context =
      std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = syscall_handler;

  if (contract.contract_type() != ContractType::kWasmi) {
    return nullptr;
  }

  void *wasmi_runtime_ptr =
      new_wasmi_runtime(contract.wasmi_contract().data(), contract.wasmi_contract().size(),
                        host_call_context.get(), wasmi_context_ptr_);
  if (wasmi_runtime_ptr == nullptr) {
    return {};
  }

  return std::make_unique<WasmiRuntime>(std::move(host_call_context),
                                        wasmi_runtime_ptr);
}

bool
WasmiContext::verify_contract(Contract const &contract) const
{
  assert(wasmi_context_ptr_);

  if (contract.contract_type() != ContractType::kWasmi) {
    return false;
  }

  return wasmi_verify_contract(wasmi_context_ptr_,
                               reinterpret_cast<const uint8_t *>(
                                   contract.wasmi_contract().data()),
                               contract.wasmi_contract().size());
}

bool
WasmiContext::link_fn_nargs(std::string_view module_name,
                            std::string_view fn_name, void *fn_ptr,
                            uint8_t nargs)
{
  assert(wasmi_context_ptr_ != nullptr);
  return wasmi_link_nargs(wasmi_context_ptr_,
                          reinterpret_cast<const uint8_t *>(module_name.data()),
                          module_name.size(),
                          reinterpret_cast<const uint8_t *>(fn_name.data()),
                          fn_name.size(), fn_ptr, nargs);
}

std::span<std::byte>
WasmiRuntime::get_memory() noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  auto slice = wasmi_get_memory(wasmi_runtime_ptr_);
  return std::as_writable_bytes(std::span<uint8_t>(slice.mem, slice.sz));
}

InvokeStatus<int32_t>
WasmiRuntime::invoke_internal(std::string_view method_name)
{
  assert(wasmi_runtime_ptr_ != nullptr);

  WasmiInvokeResult res =
      wasmi_invoke(wasmi_runtime_ptr_,
                   reinterpret_cast<const uint8_t *>(method_name.data()),
                   method_name.size());

  InvokeErrorCode wasmi_error{res.error};
  if (wasmi_error != InvokeErrorCode::kNone) {
    return InvokeError{wasmi_error};
  }
  return res.result;
}

uint64_t
WasmiRuntime::get_available_gas() const noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  return wasmi_get_available_gas(wasmi_runtime_ptr_);
}

void
WasmiRuntime::set_available_gas(uint64_t gas) noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  wasmi_set_available_gas(wasmi_runtime_ptr_, gas);
}

WasmiRuntime::WasmiRuntime(std::unique_ptr<HostCallContext> host_call_context,
                           void *wasmi_runtime_ptr)
  : Runtime(std::move(host_call_context)), wasmi_runtime_ptr_(wasmi_runtime_ptr)
{}

WasmiRuntime::~WasmiRuntime()
{
  if (wasmi_runtime_ptr_ != nullptr) {
    free_wasmi_runtime(wasmi_runtime_ptr_);
  }
}

} // namespace hogd
