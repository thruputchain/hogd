#include <cassert>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "hogd/hogexec/wasmi/bindings.h"
#include "types.hh"

namespace hogd {

WasmiContext::WasmiContext() : wasmi_context_ptr_(new_wasmi_context())
{}

WasmiContext::~WasmiContext()
{
  free_wasmi_context(wasmi_context_ptr_);
}

std::optional<WasmiRuntime>
WasmiContext::make_new_runtime(ContractBlob const &contract,
                               SystemCallHandler *syscall_handler)
{
  std::unique_ptr<HostCallContext> host_call_context =
      std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = syscall_handler;

  void *wasmi_runtime_ptr =
      new_wasmi_runtime(contract.data(), contract.size(),
                        host_call_context.get(), wasmi_context_ptr_);
  if (wasmi_runtime_ptr == nullptr) {
    return {};
  }
  return std::make_optional<WasmiRuntime>(std::move(host_call_context),
                                          wasmi_runtime_ptr);
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

std::span<const std::byte>
WasmiRuntime::get_memory() const noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  auto slice = wasmi_get_memory(wasmi_runtime_ptr_);
  return std::as_bytes(std::span<const uint8_t>(slice.mem, slice.sz));
}

InvokeResult
WasmiRuntime::invoke(uint32_t method_name, uint64_t gas_limit)
{
  auto method_name_str = std::format("{:X}", method_name);
  return invoke(method_name_str, gas_limit);
}

InvokeResult
WasmiRuntime::invoke(std::string_view method_name, uint64_t gas_limit)
{
  assert(wasmi_runtime_ptr_ != nullptr);

  // invoke does not commit changes to available gas amounts.  We assume
  // that accounting is performed elsewhere.
  // We therefore push the previous available gas amount onto the stack.
  uint64_t prev_gas_limit = wasmi_get_available_gas(wasmi_runtime_ptr_);
  wasmi_set_available_gas(wasmi_runtime_ptr_, gas_limit);

  WasmiInvokeResult res =
      wasmi_invoke(wasmi_runtime_ptr_,
                   reinterpret_cast<const uint8_t *>(method_name.data()),
                   method_name.size());

  uint64_t gas_remaining = wasmi_get_available_gas(wasmi_runtime_ptr_);

  assert(gas_remaining <= gas_limit);
  uint64_t gas_consumed = gas_limit - gas_remaining;

  // restore previous gas amount
  wasmi_set_available_gas(wasmi_runtime_ptr_, prev_gas_limit);

  InvokeErrorCode wasmi_error{res.error};
  if (wasmi_error != InvokeErrorCode::kNone) {
    return InvokeResult{.result_ = {InvokeError{wasmi_error}},
                        .gas_consumed_ = gas_consumed};
  }
  return InvokeResult{.result_ = res.result, .gas_consumed_ = gas_consumed};
}

bool
WasmiRuntime::consume_gas(GasAmount gas) noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  auto current_gas = get_available_gas();

  if (current_gas >= gas) {
    set_available_gas(current_gas - gas);
    return true;
  }

  set_available_gas(0);
  return false;
}

GasAmount
WasmiRuntime::get_available_gas() const
{
  assert(wasmi_runtime_ptr_ != nullptr);
  return wasmi_get_available_gas(wasmi_runtime_ptr_);
}

void
WasmiRuntime::set_available_gas(uint64_t gas)
{
  assert(wasmi_runtime_ptr_ != nullptr);
  wasmi_set_available_gas(wasmi_runtime_ptr_, gas);
}

WasmiRuntime::WasmiRuntime(std::unique_ptr<HostCallContext> host_call_context,
                           void *wasmi_runtime_ptr)
  : host_call_context_(host_call_context.release()),
    wasmi_runtime_ptr_(wasmi_runtime_ptr)
{
  host_call_context_->runtime = this;
}

WasmiRuntime::WasmiRuntime(WasmiRuntime &&other) noexcept
  : host_call_context_(std::move(other.host_call_context_)),
    wasmi_runtime_ptr_(other.wasmi_runtime_ptr_)
{
  other.wasmi_runtime_ptr_ = nullptr;
  host_call_context_->runtime = this;
}

WasmiRuntime &
WasmiRuntime::operator=(WasmiRuntime &&other) noexcept
{
  host_call_context_ = std::move(other.host_call_context_);
  host_call_context_->runtime = this;
  if (wasmi_runtime_ptr_ != nullptr) {
    free_wasmi_runtime(wasmi_runtime_ptr_);
  }
  wasmi_runtime_ptr_ = other.wasmi_runtime_ptr_;
  other.wasmi_runtime_ptr_ = nullptr;
  return *this;
}

WasmiRuntime::~WasmiRuntime()
{
  if (wasmi_runtime_ptr_ != nullptr) {
    free_wasmi_runtime(wasmi_runtime_ptr_);
  }
}

} // namespace hogd
