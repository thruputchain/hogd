#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/rust/wasmtime/bindings.h"

namespace hogd {

struct Contract;

class WasmtimeGlobalContext {
public:
  explicit WasmtimeGlobalContext(size_t engine_count);
  ~WasmtimeGlobalContext();

  WasmtimeGlobalContext(WasmtimeGlobalContext const &) = delete;
  WasmtimeGlobalContext(WasmtimeGlobalContext &&other) noexcept
    : global_context_ptr_(other.global_context_ptr_)
  {
    other.global_context_ptr_ = nullptr;
  }
  WasmtimeGlobalContext &operator=(WasmtimeGlobalContext const &) = delete;
  WasmtimeGlobalContext &operator=(WasmtimeGlobalContext &&other) noexcept;

  void *get_inner() const { return global_context_ptr_; }

  // Provide the functions that are made available to wasm modules(as function
  // imports). Imports in wasm are identified by "module name" and "function
  // name". For simplicity, this API implements only u64 arguments (between 0
  // and 8) and expects imported functions to return an i32 (i.e. an error
  // code from the syscall).
  //
  // Imported functions functions have a signature of
  // HostFnResult
  // function(ExternHostCallContext context, uint64_t arg0,...);
  template<typename Fn> requires supported_host_fn_ptr<Fn>
  bool link_fn(std::string_view module_name, std::string_view fn_name, Fn f)
  {
    auto erased = reinterpret_cast<ErasedFnPtr>(f);
    if (!RuntimeContext::validate_link_fn_params(module_name, fn_name, erased))
      return false;
    return link_fn_nargs(module_name, fn_name, erased,
                         HostFnTraits<std::remove_cvref_t<Fn>>::kNargs);
  }

private:
  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     ErasedFnPtr fn_ptr, uint8_t nargs);
  void *global_context_ptr_ = nullptr;
};

// Context shared between WasmtimeRuntime objects.
// Guarantee isolation between transactions
// by starting each transaction with a separate
// WasmtimeContext.  Role is to link syscall trampolines
// and provide new runtime instances.
//
// The intent with this API is to abstract away most of the details
// of wasmtime, so that later switching to a different engine, should we so
// choose, should not require significant redesign work.
//
// This should NOT be used across multiple concurrent transactions.
class WasmtimeContext : public RuntimeContext {
public:
  // module_cache can be nullptr, in which case no module cache is used.
  // module_cache must outlive this WasmtimeContext.
  WasmtimeContext(WasmtimeGlobalContext &global_context);

  // wrapper around a manual drop call on the rust side
  ~WasmtimeContext() override;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler) override;

  InvokeErrorCode verify_contract(Contract const &contract) const override;

private:
  void *wasmtime_context_ptr_ = nullptr;
};

// Wrapper around one Wasm module (one wasmtime instance). Can be invoked, and
// will call into provided host functions and pass in (as an argument) the
// provided SystemCallHandler.
class WasmtimeRuntime : public Runtime {
public:
  // should be called only from within WasmtimeContext::make_new_runtime
  WasmtimeRuntime(SystemCallHandler *syscall_handler,
                  void *wasmtime_runtime_ptr);

  ~WasmtimeRuntime() override;

  // Get module statistics (e.g., serialized module size)
  bool get_module_stats(WasmtimeModuleStats *stats_out) const noexcept;

private:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) override;

  // manipulated in rust
  void *wasmtime_runtime_ptr_;
};

} // namespace hogd
