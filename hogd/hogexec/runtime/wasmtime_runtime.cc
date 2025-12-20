#include "hogd/hogexec/runtime/wasmtime_runtime.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "account.hh"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/rust/wasmtime/bindings.h"
#include "types.hh"
#include "x23/cpumap.h"
#include "x23/format.h"
#include "x23/x23.h"

namespace hogd {

WasmtimeModuleCache::WasmtimeModuleCache(size_t capacity)
  : wasmtime_module_cache_ptr_(new_wasmtime_module_cache(capacity))
{}

WasmtimeModuleCache::~WasmtimeModuleCache()
{
  if (wasmtime_module_cache_ptr_ != nullptr) {
    free_wasmtime_module_cache(wasmtime_module_cache_ptr_);
  }
}

void
WasmtimeModuleCache::test_reset_cache(size_t capacity)
{
  assert(capacity > 0);
  if (wasmtime_module_cache_ptr_ != nullptr) {
    free_wasmtime_module_cache(wasmtime_module_cache_ptr_);
  }
  wasmtime_module_cache_ptr_ = new_wasmtime_module_cache(capacity);
}

class WasmtimeGlobalContext {
public:
  WasmtimeGlobalContext()
  {
    engine_count = 64;
    wasmtime_global_context_ptr_ = new_wasmtime_global_context(engine_count);
  }
  ~WasmtimeGlobalContext()
  {
    if (wasmtime_global_context_ptr_ != nullptr) {
      free_wasmtime_global_context(wasmtime_global_context_ptr_);
    }
  }

  void *wasmtime_global_context_ptr_;
  size_t engine_count;
};

WasmtimeContext::WasmtimeContext(WasmtimeModuleCache *module_cache)
{
  static WasmtimeGlobalContext wasmtime_global_context;
  CHECK(wasmtime_global_context.wasmtime_global_context_ptr_ != nullptr);
  
  size_t engine = x23::core_map->bucket() % wasmtime_global_context.engine_count;
  wasmtime_context_ptr_ =
      new_wasmtime_context(wasmtime_global_context.wasmtime_global_context_ptr_,
                           module_cache ? module_cache->get_inner() : nullptr,
                           engine);
}

WasmtimeContext::~WasmtimeContext()
{
  if (wasmtime_context_ptr_ != nullptr) {
    free_wasmtime_context(wasmtime_context_ptr_);
  }
}

SystemStatus<std::unique_ptr<Runtime>>
WasmtimeContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler, bool do_contract_verify)
{
  auto host_call_context = make_new_host_call_context(syscall_handler);

  // Resolve the contract to get the WASM bytes
  auto contract_res =
      syscall_handler->resolve_contract(deployed_contract_hash.hash);
  if (!contract_res.has_value()) {
    return contract_res.error();
  }

  auto const &contract = *contract_res;
  // TODO(geoff): When kWasmtime contract type is added, use that instead of
  // kWasmi For now, wasmtime contracts use the same WASM bytecode format as
  // wasmi
  if (contract->contract_type() != ContractType::kWasmi) [[unlikely]] {
    error("Unrecoverable: Attempt to access a non-wasmi contract from "
          "inside WasmtimeContext::make_new_runtime");
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }

  // Verify contract if requested
  if (do_contract_verify) {
    if (!verify_contract(*contract)) {
      return SystemError{SystemErrorCode::kSystemValidationFailure};
    }
  }

  try {
    auto const &wasm = contract->wasmi_contract().wasm;

    void *wasmtime_runtime_ptr =
        new_wasmtime_runtime(wasm.data(), static_cast<uint32_t>(wasm.size()),
                             deployed_contract_hash.hash.data(),
                             host_call_context.get(), wasmtime_context_ptr_);

    if (wasmtime_runtime_ptr == nullptr) {
      return SystemError{SystemErrorCode::kSystemUnrecoverable};
    }

    return std::make_unique<WasmtimeRuntime>(std::move(host_call_context),
                                             wasmtime_runtime_ptr);
  } catch (x23::XdrBadUnionAccess const &e) {
    error("Unrecoverable: Bad union access: {}", e.what());
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }
}

bool
WasmtimeContext::verify_contract(Contract const &contract) const
{
  assert(wasmtime_context_ptr_);

  // TODO(geoff): When kWasmtime contract type is added, use that instead of
  // kWasmi For now, wasmtime contracts use the same WASM bytecode format as
  // wasmi
  if (contract.contract_type() != ContractType::kWasmi) {
    return false;
  }

  try {
    auto const &wasm = contract.wasmi_contract().wasm;
    return wasmtime_verify_contract(wasmtime_context_ptr_,
                                    reinterpret_cast<uint8_t const *>(
                                        wasm.data()),
                                    wasm.size());
  } catch (x23::XdrBadUnionAccess const &e) {
    error("Unrecoverable: Bad union access: {}", e.what());
    return false;
  }
}

bool
WasmtimeContext::link_fn_nargs(std::string_view module_name,
                               std::string_view fn_name, void *fn_ptr,
                               uint8_t nargs)
{
  assert(wasmtime_context_ptr_ != nullptr);
  return wasmtime_link_nargs(wasmtime_context_ptr_,
                             reinterpret_cast<uint8_t const *>(
                                 module_name.data()),
                             static_cast<uint32_t>(module_name.size()),
                             reinterpret_cast<uint8_t const *>(fn_name.data()),
                             static_cast<uint32_t>(fn_name.size()), fn_ptr,
                             nargs);
}

std::span<std::byte>
WasmtimeRuntime::get_memory() noexcept
{
  assert(wasmtime_runtime_ptr_ != nullptr);
  auto slice = wasmtime_get_memory(wasmtime_runtime_ptr_);
  return std::as_writable_bytes(std::span<uint8_t>(slice.mem, slice.sz));
}

InvokeStatus<void>
WasmtimeRuntime::invoke_internal(std::string_view method_name)
{
  assert(wasmtime_runtime_ptr_ != nullptr);

  WasmtimeInvokeResult res =
      wasmtime_invoke(wasmtime_runtime_ptr_,
                      reinterpret_cast<uint8_t const *>(method_name.data()),
                      static_cast<uint32_t>(method_name.size()));

  InvokeErrorCode wasmtime_error{static_cast<InvokeErrorCode>(res.error)};
  trace("Wasmtime Invoke Result: {}",
        x23::show(static_cast<InvokeErrorCode>(res.error)));
  if (wasmtime_error != InvokeErrorCode::kInvokeNone) {
    return InvokeError{wasmtime_error};
  }
  return {};
}

uint64_t
WasmtimeRuntime::get_available_gas() const noexcept
{
  assert(wasmtime_runtime_ptr_ != nullptr);
  return wasmtime_get_available_gas(wasmtime_runtime_ptr_);
}

void
WasmtimeRuntime::set_available_gas(uint64_t gas) noexcept
{
  assert(wasmtime_runtime_ptr_ != nullptr);
  wasmtime_set_available_gas(wasmtime_runtime_ptr_, gas);
}

WasmtimeRuntime::WasmtimeRuntime(
    std::unique_ptr<HostCallContext> host_call_context,
    void *wasmtime_runtime_ptr)
  : Runtime(std::move(host_call_context)),
    wasmtime_runtime_ptr_(wasmtime_runtime_ptr)
{}

WasmtimeRuntime::~WasmtimeRuntime()
{
  if (wasmtime_runtime_ptr_ != nullptr) {
    free_wasmtime_runtime(wasmtime_runtime_ptr_);
  }
}

} // namespace hogd
