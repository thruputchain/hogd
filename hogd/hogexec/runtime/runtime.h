#pragma once

#include <memory>
#include <span>
#include <stdexcept>
#include <string>

#include "contract.hh"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

// Context to be passed into a host function call
class SystemCallHandler;
class Runtime;

// Passed in as the argument to functions invoked from Wasmi.
// The WasmiRuntime argument allows a syscall to consume gas.
struct HostCallContext {
  Runtime *runtime = nullptr;
  SystemCallHandler *syscall_handler = nullptr;
};

enum class RuntimeEngine {
  kWasmi = 0,
};

std::string engine_to_string(RuntimeEngine engine);

inline static bool
engine_has_native_metering(RuntimeEngine engine)
{
  switch (engine) {
  case RuntimeEngine::kWasmi:
    return true;
  }
  throw std::runtime_error("unknown engine");
}

// does a missing linked function cause errors at instantiation time (true)
// or are those checks deferred to runtime (false)
inline static bool
engine_has_eager_link_guards(RuntimeEngine engine)
{
  switch (engine) {
  case RuntimeEngine::kWasmi:
    return true;
  }
  throw std::runtime_error("unknown engine");
}

// Abstract class that abstracts the context associated with a sandbox runtime.
// Most sandbox implementations have some amount of shared state (i.e., a list
// of function pointers to link, or an environment allocator).
//
// We assume each RuntimeContext can be used by multiple Runtimes, but do not
// assume that (re)using RuntimeContext across transactions is deterministic,
// nor do we assume that a RuntimeContext is threadsafe.
class RuntimeContext {
public:
  RuntimeContext() = default;

  // Disable move and copy.
  RuntimeContext(RuntimeContext &&) = delete;
  RuntimeContext &operator=(RuntimeContext &&) = delete;
  RuntimeContext(const RuntimeContext &) = delete;
  RuntimeContext &operator=(const RuntimeContext &) = delete;

  virtual ~RuntimeContext() = default;

  // Provide the functions that are made available to wasm modules(as function
  // imports). Imports in wasm are identified by "module name" and "function
  // name". For simplicity, this API implements only u64 arguments (between 0
  // and 5) and expects imported functions to return an i32 (i.e. an error
  // code from the syscall).
  //
  // Imported functions functions have a signature of
  // HostFnResult
  // function(HostCallContext* context, uint64_t arg0,...);
  //
  // Context is the object contained in a WasmiRuntime. If the HostFnStatus is
  // not an error, then the uint64 is returned to the wasm module. Otherwise,
  // Wasmi unwinds the current invocation and returns a nonzero InvokeError.
  template<std::same_as<uint64_t>... Args>
  bool link_fn(std::string_view module_name, std::string_view fn_name,
               HostFnResult (*f)(HostCallContext *, Args...))
  {
    if (module_name.empty() || fn_name.empty() || (f == nullptr))
      return false;
    return link_fn_nargs(module_name, fn_name, reinterpret_cast<void *>(f),
                         (kArgCount<Args>() + ... + 0));
  }

  // Perform any required validation checks on the contract.
  // In the case of webassembly, this is the webassembly validator.
  virtual bool verify_contract(ContractBlob const &contract) const = 0;

  virtual std::unique_ptr<Runtime>
  make_new_runtime(ContractBlob const &contract,
                   SystemCallHandler *syscall_handler,
                   bool do_contract_verify) = 0;

protected:
  virtual bool link_fn_nargs(std::string_view module_name,
                             std::string_view fn_name, void *fn_ptr,
                             uint8_t nargs) = 0;

private:
  template<typename T> constexpr static auto kArgCount = [] { return 1; };
};

// create RuntimeContext with no imported functions
std::shared_ptr<RuntimeContext> make_runtime_context(RuntimeEngine engine);
// create RuntimeContext, and initialize with hogexec's imported function(s)
std::shared_ptr<RuntimeContext> make_hogexec_context(RuntimeEngine engine);

// Virtual class that abstracts the interface required for an invocable
// sandbox.  Can be created by an instance of RuntimeContext.
class Runtime {
public:
  Runtime(std::unique_ptr<HostCallContext> host_call_context);

  // Disable copy and move
  Runtime(Runtime const &) = delete;
  Runtime &operator=(Runtime const &) = delete;
  Runtime(Runtime &&other) = delete;
  Runtime &operator=(Runtime &&other) = delete;

  // The gas counter is saved before calling this method
  // (i.e. x = get_available_gas(); invoke(something); assert(x ==
  // get_available_gas()))
  InvokeResult invoke(std::string const &method_name, uint64_t gas_limit);

  // Returns FALSE if gas consumption failed.
  //
  // Wasmi maintains the gas counter internally, which it updates while
  // running a wasm contract.  It also exposes a set/get API, which this
  // function uses.  This should, e.g., be called from within a syscall. This
  // function returns false if and only if the amount of gas left in the
  // counter at the time of call is less than the input parameter.
  [[nodiscard]]
  bool consume_gas(uint64_t gas) noexcept;

  [[nodiscard]] uint64_t virtual get_available_gas() const noexcept = 0;
  virtual void set_available_gas(uint64_t gas) noexcept = 0;

  // Complex data types pass in/out of wasm via read/writes to wasm linear
  // memory.
  //
  // Requires the module to export its memory as "memory"
  // (export "memory" (memory 0))
  virtual std::span<std::byte> get_memory() noexcept = 0;
  virtual std::span<const std::byte> get_memory() const noexcept = 0;

  virtual ~Runtime() = default;

protected:
  virtual InvokeStatus<int32_t>
  invoke_internal(std::string const &method_name) = 0;

  std::unique_ptr<HostCallContext> host_call_context_;
};

} // namespace hogd
