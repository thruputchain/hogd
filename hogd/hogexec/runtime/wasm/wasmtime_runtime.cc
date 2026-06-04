#include "hogd/hogexec/runtime/wasm/wasmtime_runtime.h"
#include "hogd/hogexec/runtime/item/deployed_contract.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/rust/wasmtime/bindings.h"
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

extern "C" ContractGetResult
wasmtime_contract_get_fn(void *syscall_handler_ptr,
                         const uint8_t *hash_ptr) noexcept
{
  if (syscall_handler_ptr == nullptr || hash_ptr == nullptr) [[unlikely]] {
    error("Unrecoverable: nullptr(s) passed to wasmtime_contract_get_fn");
    return make_err(SystemErrorCode::Unrecoverable);
  }

  SystemCallHandler &syscall_handler =
      *static_cast<SystemCallHandler *>(syscall_handler_ptr);

  // TODO(geoff): this is an extra copy
  SHA256Hash hash{};
  std::memcpy(hash.data(), hash_ptr, hash.size());

  auto res = syscall_handler.resolve_contract(hash);

  if (!res.has_value()) {
    return make_err(res.error().code());
  }

  if (*res == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed in wasmtime_contract_get_fn");
    return make_err(SystemErrorCode::Unrecoverable);
  }

  if ((*res)->contract_type() != ContractType::kWasmi) [[unlikely]] {
    error("Unrecoverable: Attempt to access a non-wasmi contract from inside "
          "wasmtime_contract_get_fn");
    return make_err(SystemErrorCode::Unrecoverable);
  }

  // the try is technically unnecessary (for the ->wasmi_contract() access,
  // which is checked above); this satisfies clang-tidy
  try {
    auto const &wasm = (*res)->wasmi_contract().data;

    if (wasm.empty()) {
      return ContractGetResult{.err_code = 0, .data = nullptr, .data_len = 0};
    }
    return ContractGetResult{.err_code = 0,
                             .data = wasm.data(),
                             .data_len = wasm.size()};
  } catch (x23::XdrBadUnionAccess const &e) {
    error("Unrecoverable: Bad union access: {}", e.what());
    return make_err(SystemErrorCode::Unrecoverable);
  }
}

} // namespace

WasmtimeGlobalContext::WasmtimeGlobalContext(size_t engine_count)
{
  // Create a single global context that manages all engines internally
  global_context_ptr_ = new_wasmtime_global_context(engine_count);
  CHECK(global_context_ptr_ != nullptr);
}

WasmtimeGlobalContext::~WasmtimeGlobalContext()
{
  if (global_context_ptr_ != nullptr) {
    free_wasmtime_global_context(global_context_ptr_);
  }
}

WasmtimeGlobalContext &
WasmtimeGlobalContext::operator=(WasmtimeGlobalContext &&other) noexcept
{
  if (this == &other) {
    return *this;
  }
  if (global_context_ptr_ != nullptr) {
    free_wasmtime_global_context(global_context_ptr_);
  }

  global_context_ptr_ = other.global_context_ptr_;
  other.global_context_ptr_ = nullptr;
  return *this;
}

WasmtimeContext::WasmtimeContext(WasmtimeGlobalContext &global_context)
{
  wasmtime_context_ptr_ = new_wasmtime_context(&wasmtime_contract_get_fn,
                                               global_context.get_inner());
  CHECK(wasmtime_context_ptr_ != nullptr);
}

WasmtimeContext::~WasmtimeContext()
{
  if (wasmtime_context_ptr_ != nullptr) {
    free_wasmtime_context(wasmtime_context_ptr_);
  }
}

SystemStatus<std::unique_ptr<Runtime>>
WasmtimeContext::make_new_runtime(DeployedContract const &deployed_contract,
                                  SystemCallHandler *syscall_handler)
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
  assert(syscall_handler != nullptr);

  NewWasmtimeRuntimeResult new_runtime_result =
      new_wasmtime_runtime(deployed_contract_hash.hash.data(), syscall_handler,
                           wasmtime_context_ptr_);

  if (new_runtime_result.err_code != 0) [[unlikely]] {
    return SystemError::from_raw_error_code(new_runtime_result.err_code);
  }

  if (new_runtime_result.runtime_pointer == nullptr) {
    return nullptr;
  }

  return std::make_unique<WasmtimeRuntime>(syscall_handler,
                                           new_runtime_result.runtime_pointer);
}

InvokeErrorCode
WasmtimeContext::verify_contract(Contract const &contract) const
{
  assert(wasmtime_context_ptr_);

  // TODO(geoff): When kWasmtime contract type is added, use that instead of
  // kWasmi For now, wasmtime contracts use the same WASM bytecode format as
  // wasmi
  if (contract.contract_type() != ContractType::kWasmi) {
    return InvokeErrorCode::Deterministic;
  }

  try {
    auto const &wasm = contract.wasmi_contract().data;
    return static_cast<InvokeErrorCode>(
        wasmtime_verify_contract(wasmtime_context_ptr_,
                                 reinterpret_cast<uint8_t const *>(wasm.data()),
                                 wasm.size()));
  } catch (x23::XdrBadUnionAccess const &e) {
    error("Unrecoverable: Bad union access: {}", e.what());
    return InvokeErrorCode::Unrecoverable;
  }
}

bool
WasmtimeGlobalContext::link_fn_nargs(std::string_view module_name,
                                     std::string_view fn_name,
                                     ErasedFnPtr fn_ptr, uint8_t nargs)
{
  assert(global_context_ptr_ != nullptr);
  return wasmtime_global_link_nargs(
      global_context_ptr_,
      reinterpret_cast<uint8_t const *>(module_name.data()),
      static_cast<uint32_t>(module_name.size()),
      reinterpret_cast<uint8_t const *>(fn_name.data()),
      static_cast<uint32_t>(fn_name.size()), fn_ptr, nargs);
}

bool
WasmtimeRuntime::get_module_stats(WasmtimeModuleStats *stats_out) const noexcept
{
  assert(wasmtime_runtime_ptr_ != nullptr);
  assert(stats_out != nullptr);
  return wasmtime_get_module_stats(wasmtime_runtime_ptr_, stats_out);
}

std::pair<InvokeStatus<void>, GasAmount>
WasmtimeRuntime::invoke_internal(MethodID method_id, uint64_t gas_limit)
{
  assert(wasmtime_runtime_ptr_ != nullptr);
  uint8_t method_name[16];
  std::format_to(method_name, "{:016X}", method_id);

  WasmtimeInvokeResult res =
      wasmtime_invoke(wasmtime_runtime_ptr_, &method_name[0], 16, gas_limit);

  InvokeErrorCode wasmtime_error{static_cast<InvokeErrorCode>(res.error)};
  trace("Wasmtime Invoke Result: {}",
        x23::show(static_cast<InvokeErrorCode>(res.error)));
  if (wasmtime_error != InvokeErrorCode::None) {
    return {InvokeError{wasmtime_error}, res.gas_consumed};
  }
  return {{}, res.gas_consumed};
}

WasmtimeRuntime::WasmtimeRuntime(SystemCallHandler *syscall_handler,
                                 void *wasmtime_runtime_ptr)
  : Runtime(syscall_handler), wasmtime_runtime_ptr_(wasmtime_runtime_ptr)
{
  assert(wasmtime_runtime_ptr_ != nullptr);
}

WasmtimeRuntime::~WasmtimeRuntime()
{
  if (wasmtime_runtime_ptr_ != nullptr) {
    free_wasmtime_runtime(wasmtime_runtime_ptr_);
  }
}

} // namespace hogd
