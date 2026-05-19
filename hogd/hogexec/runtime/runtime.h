#pragma once

#include <charconv>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime_exports.h"

namespace hogd {

// Context to be passed into a host function call
class SystemCallHandler;
class Runtime;

enum class RuntimeEngine {
  kWasmi = 0,
};

namespace detail {

struct RuntimeEngineInfo {
  std::string name;
  bool has_native_metering;
  bool has_eager_link_guards;
};

constexpr static RuntimeEngineInfo kWasmiEngineInfo = {
    .name = "wasmi",
    .has_native_metering = true,
    .has_eager_link_guards = true,
};

constexpr RuntimeEngineInfo const &
get_engine_info(RuntimeEngine engine)
{
  switch (engine) {
  case RuntimeEngine::kWasmi:
    return kWasmiEngineInfo;
  }
  throw std::runtime_error("unknown engine");
}

} // namespace detail

// Passed in as the argument to functions invoked from a sandbox.
// The Runtime* argument allows a syscall to consume gas.
struct HostCallContext {
  Runtime *runtime = nullptr;
  SystemCallHandler *syscall_handler = nullptr;
};

inline std::unique_ptr<HostCallContext>
make_new_host_call_context(SystemCallHandler *syscall_handler)
{
  // ctor of Runtime is responsible for filling in out->runtime pointer.
  auto out = std::make_unique<HostCallContext>();
  out->syscall_handler = syscall_handler;
  return out;
}

std::string_view inline engine_to_string(RuntimeEngine engine)
{
  return detail::get_engine_info(engine).name;
}

// Whether or not the engine has native gas metering.
// Use case is when testing behavior across multiple engines,
// some tests will succeed/fail based on whether gas metering
// is implemented in the engine.
constexpr static bool
engine_has_native_metering(RuntimeEngine engine)
{
  return detail::get_engine_info(engine).has_native_metering;
}

// does a missing linked function cause errors at instantiation time (true)
// or are those checks deferred to runtime (false)
// Usage is again for tests across multiple wasm engines.
constexpr static bool
engine_has_eager_link_guards(RuntimeEngine engine)
{
  return detail::get_engine_info(engine).has_eager_link_guards;
}

using ErasedHostFn = void();
using ErasedFnPtr = ErasedHostFn *;

// Erased C-ABI function-pointer type for syscall trampoline dispatch.
// Matches the Rust-side common::ErasedFnPtr = unsafe extern "C" fn().
// Using a function-pointer type (not void*) keeps the cast chain within
// the guaranteed fn-ptr <-> fn-ptr reinterpret_cast
// ([expr.reinterpret.cast]/6).
template<typename Fn> struct HostFnTraits;

template<> struct HostFnTraits<HostFn0> {
  static constexpr uint8_t kNargs = 0;
};

template<> struct HostFnTraits<HostFn1> {
  static constexpr uint8_t kNargs = 1;
};

template<> struct HostFnTraits<HostFn2> {
  static constexpr uint8_t kNargs = 2;
};

template<> struct HostFnTraits<HostFn3> {
  static constexpr uint8_t kNargs = 3;
};

template<> struct HostFnTraits<HostFn4> {
  static constexpr uint8_t kNargs = 4;
};

template<> struct HostFnTraits<HostFn5> {
  static constexpr uint8_t kNargs = 5;
};

template<> struct HostFnTraits<HostFn6> {
  static constexpr uint8_t kNargs = 6;
};

template<> struct HostFnTraits<HostFn7> {
  static constexpr uint8_t kNargs = 7;
};

template<> struct HostFnTraits<HostFn8> {
  static constexpr uint8_t kNargs = 8;
};

template<> struct HostFnTraits<HostFn9> {
  static constexpr uint8_t kNargs = 9;
};

template<> struct HostFnTraits<HostFn10> {
  static constexpr uint8_t kNargs = 10;
};

template<> struct HostFnTraits<HostFn11> {
  static constexpr uint8_t kNargs = 11;
};

template<> struct HostFnTraits<HostFn12> {
  static constexpr uint8_t kNargs = 12;
};

template<typename Fn>
concept supported_host_fn_ptr =
    requires { HostFnTraits<std::remove_cvref_t<Fn>>::kNargs; };

// Abstract class that wraps the context associated with a sandbox runtime.
// Most sandbox implementations have some amount of shared state (i.e., a list
// of function pointers to link, or an environment allocator).
//
// We assume each RuntimeContext can be used by multiple Runtimes.
// Each RuntimeContext can be used across multiple transactions,
// but is not threadsafe.
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
  // and 8) and expects imported functions to return an i32 (i.e. an error
  // code from the syscall).
  //
  // Imported functions functions have a signature of
  // HostFnResult
  // function(HostCallContext* context, uint64_t arg0,...);
  //
  // Validate link_fn parameters. Returns false if any parameter is invalid.
  static bool validate_link_fn_params(std::string_view module_name,
                                      std::string_view fn_name,
                                      ErasedFnPtr fn_ptr)
  {
    return !module_name.empty() && !fn_name.empty() && (fn_ptr != nullptr);
  }

  // Perform any required validation checks on the contract.
  // In the case of webassembly, this is the webassembly validator.
  virtual bool verify_contract(Contract const &contract) const = 0;

  virtual SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler) = 0;
};

// create RuntimeContext with no imported functions
std::unique_ptr<RuntimeContext> make_runtime_context(RuntimeEngine engine);
// create RuntimeContext for running wasm contracts, and initialize with
// hogexec's imported function(s)
std::unique_ptr<RuntimeContext> make_hogexec_wasm_context(RuntimeEngine engine);

// Virtual class that abstracts the interface required for an invocable
// sandbox.  Can be created by an instance of RuntimeContext.
class Runtime {
public:
  Runtime(std::unique_ptr<HostCallContext> host_call_context);

  using MethodID =
      uint64_t; // from ContractEntryPointID, interpreted in big-endian

  // Disable copy and move
  Runtime(Runtime const &) = delete;
  Runtime &operator=(Runtime const &) = delete;
  Runtime(Runtime &&other) = delete;
  Runtime &operator=(Runtime &&other) = delete;

  // Turns a method ID (uint32) into a method name string that
  // can be invoked within wasm.
  constexpr static std::string invoke_name(MethodID method)
  {
    return std::format("{:016X}", method);
  }
  // WASM name -> Method ID (inverse of invoke_name)
  constexpr static std::optional<MethodID>
  method_id_from_name(std::string_view name)
  {
    MethodID id;
    auto result =
        std::from_chars(name.data(), name.data() + name.size(), id, 16);
    if (result.ec == std::errc() && result.ptr == name.data() + name.size()) {
      return id;
    }
    return std::nullopt;
  }

  InvokeResult invoke(std::string_view method_name, uint64_t gas_limit);
  InvokeResult invoke(MethodID entrypoint, uint64_t gas_limit)
  {
    char buf[16];
    std::format_to(buf, "{:016X}", entrypoint);
    return invoke(std::string_view(buf, 16), gas_limit);
  }

  virtual ~Runtime() = default;

protected:
  // Invariant:  The returned GasAmount must be <= the input gas_limit
  virtual std::pair<InvokeStatus<void>, GasAmount> invoke_internal(std::string_view method_name, uint64_t gas_limit) = 0;

  std::unique_ptr<HostCallContext> host_call_context_;
};

} // namespace hogd
