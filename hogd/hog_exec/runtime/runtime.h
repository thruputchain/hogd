#pragma once

#include <charconv>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"

namespace hogd {

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
  panic("unknown engine");
}

} // namespace detail

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
  // function(ExternHostCallContext context, uint64_t arg0,...);
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
  virtual InvokeErrorCode verify_contract(Contract const &contract) const = 0;

  virtual SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) = 0;
};

// create RuntimeContext with no imported functions
std::unique_ptr<RuntimeContext> make_runtime_context(RuntimeEngine engine);
// create RuntimeContext for running wasm contracts, and initialize with
// hog_exec's imported function(s)
std::unique_ptr<RuntimeContext>
make_hog_exec_wasm_context(RuntimeEngine engine);

// Virtual class that abstracts the interface required for an invocable
// sandbox.  Can be created by an instance of RuntimeContext.
class Runtime {
public:
  // `syscall_handler` is non-owning and must outlive the Runtime. It is the
  // opaque context the sandbox bakes in and hands back to syscall_entry.
  explicit Runtime(SystemCallHandler *syscall_handler);

  using MethodID =
      uint64_t; // from ContractEntryPointID, interpreted in big-endian

  // Disable copy and move
  Runtime(Runtime const &) = delete;
  Runtime &operator=(Runtime const &) = delete;
  Runtime(Runtime &&other) = delete;
  Runtime &operator=(Runtime &&other) = delete;

  // Writes the 16-byte Wasm method export name for `method` into `out`.
  //
  // The hex digits are emitted from a `u8"..."` literal so the bytes are the
  // standard-mandated UTF-8 (== ASCII) encoding regardless of the ordinary
  // execution character set (`-fexec-charset`). Rust decodes this name as UTF-8
  // before the Wasm export lookup, and the resulting dispatch feeds the
  // consensus event root, so an ordinary `std::format("{:016X}", ...)` would
  // make protocol-visible dispatch depend on implementation-defined literal
  // bytes that can diverge across honest builds. See hogd issue #2722.
  //
  // This fills a caller-provided stack buffer and performs no allocation, so it
  // is safe on the noexcept child-Invoke dispatch path where a std::bad_alloc
  // would otherwise cross the noexcept boundary and terminate.
  constexpr static void write_invoke_name(MethodID method, uint8_t (&out)[16])
  {
    constexpr std::u8string_view kHexDigits = u8"0123456789ABCDEF";
    for (size_t i = 0; i < 16; ++i) {
      auto const nibble = (method >> ((15 - i) * 4)) & 0xFU;
      out[i] = static_cast<uint8_t>(kHexDigits[nibble]);
    }
  }

  // Turns a method ID into the 16-byte method name string that is invoked
  // within wasm. Prefer write_invoke_name() on hot or noexcept paths to avoid
  // the allocation.
  constexpr static std::string invoke_name(MethodID method)
  {
    uint8_t buf[16];
    write_invoke_name(method, buf);
    std::string name(16, '\0');
    for (size_t i = 0; i < 16; ++i) {
      name[i] = static_cast<char>(buf[i]);
    }
    return name;
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

  InvokeResult invoke(MethodID method_id, uint64_t gas_limit);

  virtual ~Runtime() = default;

protected:
  // Invariant:  The returned GasAmount must be <= the input gas_limit
  virtual std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) = 0;

  SystemCallHandler *syscall_handler_ = nullptr;
};

} // namespace hogd
