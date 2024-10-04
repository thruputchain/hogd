#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "hogd/hogexec/runtime/status.h"
#include "types.hh"

namespace hogd {

// Context to be passed into a host function call
class SystemCallHandler;
class WasmiRuntime;

// Passed in as the argument to functions invoked from Wasmi.
// The WasmiRuntime argument allows a syscall to consume gas.
struct HostCallContext {
  WasmiRuntime *runtime = nullptr;
  SystemCallHandler *syscall_handler = nullptr;
};

struct InvokeResult {
  InvokeStatus<uint64_t> result;
  uint64_t gas_consumed;
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
// This should NOT be used across multiple concurrent transactions.
// The internal wasmi::EngineInner has shared state,
// which (appears to be) appropriately protected so as to be
// threadsafe.
// HOWEVER, contention on this structure could produce weird performance
// artifacts, (every access is protected by a mutex, and I'm not convinced it
// doesn't hold this mutex for too long sometimes: see
// https://github.com/wasmi-labs/wasmi/pull/1122) and crucially, in lazy
// compilation/translation mode, there are code artifacts that get put into the
// internal engine structure and checks for these structures affect gas
// metering. From the changelog: ``Note that CompilationMode::Lazy can lead to
// partially validated Wasm modules which can introduce non-determinism when
// using different Wasm implementations. Therefore users should know what they
// are doing when using CompilationMode::Lazy if this is a concern.'' I suspect
// that this would be ok, if we used a separately instantiated wasmi::Module for
// each module.  But I would rather not worry about the internals of wasmi this
// much.
//
// Wasmi internally manages its own lifetimes, so it is safe to
// have this be destructed before all WasmiRuntimes that it created
// are destroyed.
class WasmiContext {
public:
  WasmiContext();
  WasmiContext(WasmiContext const &) = delete;
  ~WasmiContext();

  // Provide the functions that are made available to wasm modules (as function
  // imports). Imports in wasm are identified by "module name" and "function
  // name". For simplicity, this API implements only u64 arguments (between 0
  // and 5) and expects imported functions to return a u64 (i.e. an error code
  // from the syscall).
  //
  // These functions have a signature of
  // HostFnStatus<uint64_t>
  // function(HostCallContext* context, uint64_t arg0,...);
  //
  // Context is the object contained in a WasmiRuntime.
  // If the HostFnStatus is not an error, then the uint64 is returned to the
  // wasm module. Otherwise, Wasmi unwinds the current invocation and returns a
  // nonzero WasmiInvokeError.
  template<std::same_as<uint64_t>... Args>
  bool link_fn(std::string_view module_name, std::string_view fn_name,
               HostFnStatus<uint64_t> (*f)(HostCallContext *, Args...))
  {
    return link_fn_nargs(module_name, fn_name, reinterpret_cast<void *>(f),
                         (kArgCount<Args>() + ... + 0));
  }

  std::unique_ptr<WasmiRuntime>
  make_new_runtime(ContractBlob const &contract,
                   SystemCallHandler *syscall_handler);

private:
  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     void *fn_ptr, uint8_t nargs);

  template<typename T> constexpr static auto kArgCount = [] { return 1; };

  void *wasmi_context_ptr_;
};

// Wrapper around one Wasm module (one wasmi instance).
// Can be invoked, and will call into provided host functions
// and pass in (as an argument) the provided SystemCallHandler.
class WasmiRuntime {
public:
  // should be called only from within WasmiContext::make_new_runtime
  WasmiRuntime(std::unique_ptr<HostCallContext> host_call_context,
               void *wasmi_runtime_ptr);
  WasmiRuntime(WasmiRuntime const &) = delete;
  ~WasmiRuntime();

  // This API expects that the invoked function takes no arguments
  // and returns a uint64_t.
  // Syscalls will handle "returning" larger values from one smart contract
  // to another.  The uint64 should be interpreted as an error code,
  // which could be defined by the smart contract (just as system calls
  // will return a uint64 error -- which is why link_fn demands a function
  // with return type HostFnStatus<uint64>)
  //
  // The gas counter is saved before calling this method
  // (i.e. x = get_available_gas(); invoke(something); assert(x ==
  // get_available_gas()))
  InvokeResult invoke(std::string_view method_name, uint64_t gas_limit);

  // Returns FALSE if gas consumption failed.
  //
  // Wasmi maintains the gas counter internally, which it updates while
  // running a wasm contract.  It also exposes a set/get API, which this
  // function uses.  This should, e.g., be called from within a syscall. This
  // function returns false if and only if the amount of gas left in the counter
  // at the time of call is less than the input parameter.
  [[nodiscard]]
  bool consume_gas(uint64_t gas) noexcept;

  [[nodiscard]] uint64_t get_available_gas() const;
  void set_available_gas(uint64_t gas);

  // Complex data types pass in/out of wasm via read/writes to
  // wasm linear memory.
  //
  // Requires the module to export its memory as "memory"
  // (export "memory" (memory 0))
  std::span<std::byte> get_memory() noexcept;
  std::span<const std::byte> get_memory() const noexcept;

private:
  std::unique_ptr<HostCallContext> host_call_context_;
  // manipulated in rust
  void *wasmi_runtime_ptr_;
};

} // namespace hogd
