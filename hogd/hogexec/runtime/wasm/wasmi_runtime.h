#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

struct Contract;

class WasmiGlobalContext {
public:
  WasmiGlobalContext();
  ~WasmiGlobalContext();

  WasmiGlobalContext(WasmiGlobalContext const &) = delete;

  WasmiGlobalContext(WasmiGlobalContext &&other) noexcept
    : global_context_ptr_(other.global_context_ptr_)
  {
    other.global_context_ptr_ = nullptr;
  }

  WasmiGlobalContext &operator=(WasmiGlobalContext const &) = delete;
  WasmiGlobalContext &operator=(WasmiGlobalContext &&other) noexcept;

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

// Context shared between WasmiRuntime objects.
// Guarantee isolation between transactions
// by starting each transaction with a separate
// WasmiContext.  Role is to link syscall trampolines
// and provide new runtime instances.
//
// The intent with this API is to abstract away most of the details
// of wasmi, so that later switching to a different engine, should we so choose,
// should not require significant redesign work.
//
// This should NOT be used across multiple concurrent transactions. The
// internal wasmi::EngineInner has shared state, which (appears to be)
// appropriately protected so as to be threadsafe.
//
// HOWEVER, contention on
// this structure could produce weird performance artifacts, (every access is
// protected by a mutex, and I'm not convinced it doesn't hold this mutex for
// too long sometimes: see https://github.com/wasmi-labs/wasmi/pull/1122) and
// crucially, in lazy compilation/translation mode, there are code artifacts
// that get put into the internal engine structure and checks for these
// structures affect gas metering. From the changelog: ``Note that
// CompilationMode::Lazy can lead to partially validated Wasm modules which
// can introduce non-determinism when using different Wasm implementations.
// Therefore users should know what they are doing when using
// CompilationMode::Lazy if this is a concern.'' I suspect that this would be
// ok, if we used a separately instantiated wasmi::Module for each module.
// But I would rather not worry about the internals of wasmi this much.
//
// Wasmi internally manages its own lifetimes, so it is safe to
// have this be destructed before all WasmiRuntimes that it created
// are destroyed.
class WasmiContext : public RuntimeContext {
public:
  WasmiContext(WasmiGlobalContext &global_context);

  // wrapper around a manual drop call on the rust side
  ~WasmiContext() override;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler) override;

  InvokeErrorCode verify_contract(Contract const &contract) const override;

private:
  WasmiGlobalContext &global_context_;
  void *wasmi_context_ptr_;
};

// Wrapper around one Wasm module (one wasmi instance). Can be invoked, and
// will call into provided host functions and pass in (as an argument) the
// provided SystemCallHandler.
class WasmiRuntime : public Runtime {
public:
  // should be called only from within WasmiContext::make_new_runtime
  WasmiRuntime(SystemCallHandler *syscall_handler, void *wasmi_runtime_ptr);

  ~WasmiRuntime() override;

private:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) override;

  // manipulated in rust
  void *wasmi_runtime_ptr_;
};

} // namespace hogd
