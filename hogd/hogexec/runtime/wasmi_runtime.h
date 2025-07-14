#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "types.hh"

namespace hogd {

struct Contract;

// Cache wasm modules across multiple contexts.
// Can be used across multiple WasmiContexts and across multiple transactions.
//
// === WARNINGS ===
// MUST OUTLIVE ANY WasmiContexts that use it.
// MUST NOT BE USED WITH CompilationMode::Lazy or
// CompilationMode::LazyTranslation in Wasmi!!!
class WasmiModuleCache {
public:
  WasmiModuleCache(size_t capacity);
  ~WasmiModuleCache();

  // Disable copy and move
  WasmiModuleCache(WasmiModuleCache const &) = delete;
  WasmiModuleCache &operator=(WasmiModuleCache const &) = delete;
  WasmiModuleCache(WasmiModuleCache &&) = delete;
  WasmiModuleCache &operator=(WasmiModuleCache &&) = delete;

private:
  void *wasmi_module_cache_ptr_;
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
  // module_cache can be nullptr, in which case no module cache is used.
  // module_cache must outlive this WasmiContext.
  WasmiContext(WasmiModuleCache *module_cache = nullptr);

  // wrapper around a manual drop call on the rust side
  ~WasmiContext() override;

  std::unique_ptr<Runtime> make_new_runtime(Contract const &contract,
                                            SHA256Hash const &hash,
                                            SystemCallHandler *syscall_handler,
                                            bool do_contract_verify) override;

  bool verify_contract(Contract const &contract) const override;

private:
  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     void *fn_ptr, uint8_t nargs) override;

  void *wasmi_context_ptr_;
};

// Wrapper around one Wasm module (one wasmi instance). Can be invoked, and
// will call into provided host functions and pass in (as an argument) the
// provided SystemCallHandler.
class WasmiRuntime : public Runtime {
public:
  // should be called only from within WasmiContext::make_new_runtime
  WasmiRuntime(std::unique_ptr<HostCallContext> host_call_context,
               void *wasmi_runtime_ptr);

  ~WasmiRuntime() override;

  [[nodiscard]] uint64_t get_available_gas() const noexcept override;
  void set_available_gas(uint64_t gas) noexcept override;

  // Complex data types pass in/out of wasm via read/writes to wasm linear
  // memory.
  //
  // Requires the module to export its memory as "memory"
  // (export "memory" (memory 0))
  std::span<std::byte> get_memory() noexcept override;

private:
  InvokeStatus<void> invoke_internal(std::string_view method_name) override;

  // manipulated in rust
  void *wasmi_runtime_ptr_;
};

} // namespace hogd
