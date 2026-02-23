#pragma once

#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "syscall.hh"
#include "syscall_error.hh"

// Include the generated syscall_meta.h
#include "hogd/hogexec/runtime/syscall_meta.h"

namespace hogd {

// Routes native contract syscalls through wrap_after_parse, so they get
// read-only enforcement, tracing, and gas accounting -- just like wasm
// contracts.
class NativeSyscallContext {
public:
  NativeSyscallContext(SystemCallHandler &h, NativeRuntime &r)
    : handler_(h), runtime_(r)
  {}

  template<SyscallNo Syscall, typename... Args>
  HostFnStatus<SyscallErrorCode> syscall(Args &&...args)
  {
    using Meta = SyscallMeta<Syscall>;
    using syscall_t =
        SyscallTypes<std::remove_const_t<decltype(Meta::kInnerFunc)>>;
    static_assert(sizeof...(Args) == syscall_t::kNumArgs);

    auto parsed = [&]<size_t... Is>(std::index_sequence<Is...>) {
      return std::tuple{
          to_parsed<typename syscall_t::template arg_i<Is>>(
              std::forward<Args>(args))...};
    }(std::make_index_sequence<sizeof...(Args)>{});

    return wrap_after_parse<Meta>(handler_, runtime_, Meta::kSyscallNo,
                                  parsed);
  }

  SystemCallHandler &handler() { return handler_; }
  NativeRuntime &runtime() { return runtime_; }

private:
  SystemCallHandler &handler_;
  NativeRuntime &runtime_;

  // Convert a native arg to the parsed form that wrap_after_parse expects.
  //
  // Output refs (T&): wrap in WritableOutput<T> pointing at the caller's
  //   variable.  The RAII destructor copies the result back on scope exit.
  //
  // Optional pointers (const T*): wrap in OptionalInputParseResult.
  //
  // Everything else (values, input refs, spans): wrap in std::optional.
  template<typename ArgParam, typename Arg>
  static auto to_parsed(Arg &&arg)
  {
    if constexpr (std::is_lvalue_reference_v<ArgParam> &&
                  !std::is_const_v<std::remove_reference_t<ArgParam>>) {
      // Output ref: T&
      using T = std::remove_reference_t<ArgParam>;
      return WritableOutput<T>(static_cast<void *>(&arg));
    }
    else if constexpr (std::is_pointer_v<ArgParam> &&
                      std::is_const_v<std::remove_pointer_t<ArgParam>>) {
      // Optional input pointer: const T*
      using T = std::remove_const_t<std::remove_pointer_t<ArgParam>>;
      return typename SyscallArgParams<ArgParam>::OptionalInputParseResult{
          .is_valid = true,
          .value = arg ? std::optional<T>(*arg) : std::nullopt};
    }
    else {
      // Value types, input refs, spans: wrap in optional
      return std::optional{std::forward<Arg>(arg)};
    }
  }

};

} // namespace hogd
