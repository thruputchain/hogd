#pragma once

#include <charconv>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"

namespace hogd {

class SystemCallHandler;
class Runtime;

// Erased C-ABI function-pointer type for syscall trampoline dispatch.
// Matches the Rust-side common::ErasedFnPtr = unsafe extern "C" fn().
// Using a function-pointer type (not void*) keeps the cast chain within
// the guaranteed fn-ptr <-> fn-ptr reinterpret_cast
// ([expr.reinterpret.cast]/6).
//
// The aliases sit inside a linkage-specification so the aliased function type
// has C language linkage ([dcl.link]/4), matching both the Rust declaration
// and the HostFnN family in runtime_exports.h that this type carries.
// Language linkage is part of a function type, so without the specification
// the carrier is formally a distinct type from the C-linkage parameter the
// generated FFI headers declare -- a distinction the compilers we build with
// ignore, but one worth not relying on in the syscall dispatch path.
extern "C" {
using ErasedHostFn = void();
using ErasedFnPtr = ErasedHostFn *;
}

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

  // Perform any required validation checks on the contract.
  // In the case of webassembly, this is the webassembly validator.
  virtual InvokeErrorCode verify_contract(Contract const &contract) const = 0;

  virtual SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) = 0;
};

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
