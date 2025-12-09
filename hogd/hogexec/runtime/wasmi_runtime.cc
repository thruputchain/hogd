#include "hogd/hogexec/runtime/wasmi_runtime.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include "account.hh"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/module_cache.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/rust/wasmi/bindings.h"
#include "types.hh"
#include "x23/format.h"

namespace hogd {

namespace {

struct FFIModuleCacheEntryDeleter {
  void operator()(void **ptr)
  {
    assert(ptr != nullptr);
    if (*ptr != nullptr) {
      free_module_cache_entry(*ptr);
    }
  }
};
void
free_external_module_cache_entry(void *ptr)
{
  if (ptr != nullptr) {
    auto *entry = static_cast<std::shared_ptr<void *> *>(ptr);
    delete entry;
  }
}

int32_t
add_cached_wasmi_module(void *module_cache_ptr, const uint8_t *hash,
                        void *module_ptr) noexcept
{
  void **ptr_alloc = nullptr;
  try {
    assert(module_cache_ptr != nullptr);
    assert(hash != nullptr);
    assert(module_ptr != nullptr);
    auto const *h = reinterpret_cast<const SHA256Hash *>(hash);
    auto *module_cache = reinterpret_cast<ModuleCache *>(module_cache_ptr);
    ptr_alloc = new void *(module_ptr);
    module_cache->put(*h, *new std::shared_ptr<void *>(
                              ptr_alloc, FFIModuleCacheEntryDeleter()));
  } catch (std::bad_alloc const &e) {
    delete ptr_alloc;
    return std::to_underlying(SystemErrorCode::kSystemUnrecoverable);
  }
  return 0;
}

ModuleCacheGetResult
get_cached_wasmi_module(void *module_cache_ptr, const uint8_t *hash)
{
  assert(module_cache_ptr != nullptr);
  assert(hash != nullptr);
  auto const *h = reinterpret_cast<const SHA256Hash *>(hash);
  auto *module_cache = reinterpret_cast<ModuleCache *>(module_cache_ptr);
  auto r = module_cache->get(*h);
  if (r == nullptr) {
    return ModuleCacheGetResult{
        .entry = nullptr,
        .free_context = nullptr,
    };
  }
  auto *dup = new std::shared_ptr<void *>(r);
  return ModuleCacheGetResult{
      .entry = *r,
      .free_context = dup,
  };
}

} // namespace

WasmiContext::WasmiContext(ModuleCache *module_cache)
  : wasmi_context_ptr_(new_wasmi_context(module_cache, &get_cached_wasmi_module,
                                         &add_cached_wasmi_module,
                                         &free_external_module_cache_entry))
{}

WasmiContext::~WasmiContext()
{
  if (wasmi_context_ptr_ != nullptr) {
    free_wasmi_context(wasmi_context_ptr_);
  }
}

SystemStatus<std::unique_ptr<Runtime>>
WasmiContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler, bool do_contract_verify)
{
  auto host_call_context = make_new_host_call_context(syscall_handler);

  auto contract =
      syscall_handler->resolve_contract(deployed_contract_hash.hash);
  if (!contract.has_value()) [[unlikely]] {
    return contract.error();
  }
  if (contract.value() == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed: {}",
          deployed_contract_hash.hash);
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }

  if (contract.value()->contract_type() != ContractType::kWasmi) [[unlikely]] {
    error("Unrecoverable: Attempt made to access a non-wasmi contract from "
          "WasmiContext: {}",
          contract.value()->contract_type());
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }

  if (do_contract_verify && !verify_contract(*contract.value())) {
    return {};
  }

  NewWasmiRuntimeResult result =
      new_wasmi_runtime(contract.value()->wasmi_contract().wasm.data(),
                        contract.value()->wasmi_contract().wasm.size(),
                        deployed_contract_hash.hash.data(),
                        host_call_context.get(), wasmi_context_ptr_);
  if (result.error != 0) {
    return SystemError{static_cast<SystemErrorCode>(result.error)};
  }
  auto *wasmi_runtime_ptr = result.runtime;
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
                                   contract.wasmi_contract().wasm.data()),
                               contract.wasmi_contract().wasm.size());
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
  if (wasmi_error != InvokeErrorCode::kInvokeNone) {
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
