#include "hogd/hogexec/runtime/wasm/wasmi_runtime.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/rust/wasmi/bindings.h"
#include "types.hh"
#include "x23/format.h"
#include "x23/x23.h"

namespace hogd {

namespace {

constexpr ContractGetResult
make_err(SystemErrorCode err)
{
  return ContractGetResult{.err_code = std::to_underlying(err),
                           .data = nullptr,
                           .data_len = 0};
}

ContractGetResult
wasmi_contract_get_fn(void *host_call_context_ptr,
                      const uint8_t *hash_ptr) noexcept
{
  if (host_call_context_ptr == nullptr || hash_ptr == nullptr) [[unlikely]] {
    error("Unrecoverable: nullptr(s) passed to wasmi_contract_get_fn");
    return make_err(SystemErrorCode::Unrecoverable);
  }

  auto *host_call_context =
      static_cast<HostCallContext *>(host_call_context_ptr);
  if (host_call_context->syscall_handler == nullptr) [[unlikely]] {
    error("Unrecoverable: nullptr syscall_handler in wasmi_contract_get_fn");
    return make_err(SystemErrorCode::Unrecoverable);
  }
  SystemCallHandler &syscall_handler = *host_call_context->syscall_handler;

  // TODO(geoff): this is an extra copy
  SHA256Hash hash{};
  std::memcpy(hash.data(), hash_ptr, hash.size());

  auto res = syscall_handler.resolve_contract(hash);

  if (!res.has_value()) {
    return make_err(res.error().code());
  }

  if ((*res)->contract_type() != ContractType::kWasmi) [[unlikely]] {
    error("Unrecoverable: Attempt to access a non-wasmi contract from inside "
          "wasmi_contract_get_fn");
    return make_err(SystemErrorCode::Unrecoverable);
  }

  try {
    auto const &wasm = (*res)->wasmi_contract().wasm;

    return ContractGetResult{.err_code = 0,
                             .data = wasm.data(),
                             .data_len = wasm.size()};
  } catch (x23::XdrBadUnionAccess const &e) {
    error("Unrecoverable: Bad union access: {}", e.what());
    return make_err(SystemErrorCode::Unrecoverable);
  }
}

} // namespace

WasmiGlobalContext::WasmiGlobalContext()
{
  global_context_ptr_ = new_wasmi_global_context();
  CHECK(global_context_ptr_ != nullptr);
}

WasmiGlobalContext::~WasmiGlobalContext()
{
  if (global_context_ptr_ != nullptr) {
    free_wasmi_global_context(global_context_ptr_);
  }
}

WasmiGlobalContext &
WasmiGlobalContext::operator=(WasmiGlobalContext &&other) noexcept
{
  if (this == &other) {
    return *this;
  }
  if (global_context_ptr_ != nullptr) {
    free_wasmi_global_context(global_context_ptr_);
  }
  global_context_ptr_ = other.global_context_ptr_;
  other.global_context_ptr_ = nullptr;
  return *this;
}

bool
WasmiGlobalContext::link_fn_nargs(std::string_view module_name,
                                  std::string_view fn_name, ErasedFnPtr fn_ptr,
                                  uint8_t nargs)
{
  assert(global_context_ptr_ != nullptr);
  return wasmi_global_link_nargs(
      global_context_ptr_,
      reinterpret_cast<const uint8_t *>(module_name.data()), module_name.size(),
      reinterpret_cast<const uint8_t *>(fn_name.data()), fn_name.size(),
      fn_ptr, nargs);
}

WasmiContext::WasmiContext(WasmiGlobalContext &global_context)
  : global_context_(global_context),
    wasmi_context_ptr_(
        new_wasmi_context(&wasmi_contract_get_fn, global_context_.get_inner()))
{
  CHECK(wasmi_context_ptr_ != nullptr);
}

WasmiContext::~WasmiContext()
{
  if (wasmi_context_ptr_ != nullptr) {
    free_wasmi_context(wasmi_context_ptr_);
  }
}

SystemStatus<std::unique_ptr<Runtime>>
WasmiContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler)
{
  auto host_call_context = make_new_host_call_context(syscall_handler);

  NewWasmiRuntimeResult new_runtime_result =
      new_wasmi_runtime(deployed_contract_hash.hash.data(),
                        host_call_context.get(), wasmi_context_ptr_);

  if (new_runtime_result.err_code != 0) {
    return SystemError::from_raw_error_code(new_runtime_result.err_code);
  }

  void *wasmi_runtime_ptr = new_runtime_result.context_pointer;

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
  try {
    return wasmi_verify_contract(wasmi_context_ptr_,
                                 reinterpret_cast<const uint8_t *>(
                                     contract.wasmi_contract().wasm.data()),
                                 contract.wasmi_contract().wasm.size());
  } catch (x23::XdrBadUnionAccess const &e) {
    error("Unrecoverable: Bad union access: {}", e.what());
    return false;
  }
}

std::span<std::byte>
WasmiRuntime::get_memory() noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  auto slice = wasmi_get_memory(wasmi_runtime_ptr_);
  return std::as_writable_bytes(std::span<uint8_t>(slice.mem, slice.sz));
}

InvokeStatus<void>
WasmiRuntime::invoke_internal(std::string_view method_name)
{
  assert(wasmi_runtime_ptr_ != nullptr);

  WasmiInvokeResult res =
      wasmi_invoke(wasmi_runtime_ptr_,
                   reinterpret_cast<const uint8_t *>(method_name.data()),
                   method_name.size());

  InvokeErrorCode wasmi_error{res.error};
  trace("Wasmi Invoke Result: {}",
        x23::show(static_cast<InvokeErrorCode>(res.error)));
  if (wasmi_error != InvokeErrorCode::None) {
    return InvokeError{wasmi_error};
  }
  return {};
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
