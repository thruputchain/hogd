#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/alias_guard.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/syscall_arg_params.h"
#include "hogd/hog_exec/runtime/syscall_argument_parsers.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_meta.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

// Routes native contract syscalls through wrap_after_parse, so they get
// read-only enforcement, tracing, and gas accounting -- just like guest
// contracts.
class NativeSyscallContext {
public:
  NativeSyscallContext(SystemCallHandler &h, GasAmount gas_limit)
    : handler_(h), initial_gas_amount_(gas_limit)
  {}

  // A coroutine: the syscall it routes to may suspend on a database read, so
  // native contract bodies await this rather than blocking a thread on it.
  template<SyscallNo Syscall, typename... Args>
  [[nodiscard]] x23::Async<HostFnStatus<SyscallErrorCode>>
  syscall(Args &&...args)
  {
    using Meta = SyscallMeta<Syscall>;
    using syscall_t =
        SyscallTypes<std::remove_const_t<decltype(Meta::kInnerFunc)>>;
    static_assert(sizeof...(Args) == syscall_t::kNumArgs);

    AliasGuard guard;

    auto parsed = [&]<size_t... Is>(std::index_sequence<Is...>) {
      // Braced so the arguments register with the guard left to right, as
      // the other two entry paths do.
      return std::tuple{to_parsed<typename syscall_t::template arg_i<Is>>(
          guard, std::forward<Args>(args))...};
    }(std::make_index_sequence<sizeof...(Args)>{});

    auto [res, gas_consumed] =
        co_await wrap_after_parse<Meta>(guard.check(), handler_,
                                        gas_remaining(), Meta::kSyscallNo,
                                        std::move(parsed));

    // The syscall ran within gas_remaining(), so its result already reports
    // anything it could not pay for; a frame left exactly empty is upgraded to
    // OutOfGas when it finishes (NativeContract::invoke_impl).
    std::ignore = consume_gas(gas_consumed);
    co_return res;
  }

  SystemCallHandler &handler() { return handler_; }

  // Read [address]'s native token balance, charging this context for any
  // database load it does. The native token precompile reaches the handler
  // directly instead of going through syscall(), so this is where that load
  // gets billed; call it rather than the handler's method.
  //
  // Out of line: an inline coroutine here would emit a body referencing the
  // handler into every translation unit that includes this header, including
  // ones whose libraries do not link the runtime.
  [[nodiscard]] x23::Async<SystemStatus<SyscallStatus<int128_t>>>
  native_token_balance(ContractAddress const &address);

  // Apply [amount] to [address]'s native token balance, charging this context
  // a flat write cost up front; see native_token_balance above.
  [[nodiscard]] x23::Async<SystemStatus<SyscallStatus<void>>>
  modify_native_token_balance(ContractAddress const &address, int128_t amount);

  GasAmount gas_remaining() const
  {
    if (gas_consumed_ > initial_gas_amount_) {
      return 0;
    }
    return initial_gas_amount_ - gas_consumed_;
  }

  GasAmount total_gas_consumed() const
  {
    return initial_gas_amount_ - gas_remaining();
  }

  // Debits [amount], saturating at the limit. False once nothing is left: a
  // caller that goes on to do more work must stop there.
  [[nodiscard]] bool consume_gas(GasAmount amount)
  {
    auto [sum, of] = overflowing_add(amount, gas_consumed_);

    if (of) {
      gas_consumed_ = std::numeric_limits<GasAmount>::max();
    }
    else {
      gas_consumed_ = sum;
    }

    return initial_gas_amount_ > gas_consumed_;
  }

private:
  SystemCallHandler &handler_;
  const GasAmount initial_gas_amount_;
  GasAmount gas_consumed_ = 0;

  // Convert a native arg to the parsed form that wrap_after_parse expects.
  //
  // Output refs (T&): wrap in WritableOutput<T> pointing at the caller's
  //   variable.  The RAII destructor copies the result back on scope exit.
  //
  // Optional pointers (const T*): wrap in OptionalInputParseResult.
  //
  // Everything else (values, input refs, spans): wrap in std::optional.
  //
  // Memory-naming args register with [guard] as inputs or outputs, as parse()
  // and parse_mock() do; a null optional input registers nothing.
  template<typename ArgParam, typename Arg>
  static auto to_parsed(AliasGuard &guard, Arg &&arg)
  {
    if constexpr (std::is_lvalue_reference_v<ArgParam> &&
                  !std::is_const_v<std::remove_reference_t<ArgParam>>) {
      // Output ref: T&
      using T = std::remove_reference_t<ArgParam>;
      // The arity static_assert in syscall() only checks the *number* of
      // arguments, not their types.  WritableOutput<T> writes sizeof(T) bytes
      // back into &arg, so if the caller passes storage of a different type
      // the static_cast<void *> below would silently reinterpret it -- an
      // out-of-bounds / type-confusing write with no compile or runtime check.
      // Require the caller's argument type to exactly match the declared
      // output type, so any mismatch fails to compile instead.
      static_assert(std::is_same_v<std::remove_cvref_t<Arg>, T>,
                    "native syscall output argument type must exactly match "
                    "the syscall's declared output parameter type");
      register_output(guard, object_bytes(arg));
      return WritableOutput<T>(static_cast<void *>(&arg));
    }
    else if constexpr (std::is_pointer_v<ArgParam> &&
                       std::is_const_v<std::remove_pointer_t<ArgParam>>) {
      // Optional input pointer: const T*
      using T = std::remove_const_t<std::remove_pointer_t<ArgParam>>;
      if (arg) {
        register_input(guard, object_bytes(*arg));
      }
      return typename SyscallArgParams<ArgParam>::OptionalInputParseResult{
          .is_valid = true,
          .value = arg ? std::optional<T>(*arg) : std::nullopt};
    }
    else if constexpr (x23::is_bytespan<ArgParam>) {
      using Span = std::remove_cvref_t<ArgParam>;
      if constexpr (std::is_const_v<typename Span::element_type>) {
        auto bytes = std::as_bytes(std::span(arg));
        register_input(guard, bytes);
        return std::optional{bytes};
      }
      else {
        auto bytes = std::as_writable_bytes(std::span(arg));
        register_output(guard, bytes);
        return std::optional{bytes};
      }
    }
    else {
      if constexpr (std::is_lvalue_reference_v<ArgParam>) {
        // Input ref: const T&. The body gets a copy, but the guest and mock
        // paths register the caller's bytes all the same, so this does too.
        register_input(guard, object_bytes(arg));
      }
      // Value types: wrap in optional
      return std::optional{std::forward<Arg>(arg)};
    }
  }

  // The bytes of a by-reference argument, under the requirement parse() and
  // parse_mock() place on one.
  template<typename T> requires (std::is_trivially_copyable_v<T> &&
                                 std::has_unique_object_representations_v<T>)
  static std::span<const std::byte> object_bytes(T const &object)
  {
    return std::as_bytes(std::span<const T, 1>(std::addressof(object), 1));
  }

  static void register_input(AliasGuard &guard,
                             std::span<const std::byte> bytes)
  {
    guard.check_and_register_input(reinterpret_cast<uintptr_t>(bytes.data()),
                                   bytes.size());
  }

  static void register_output(AliasGuard &guard,
                              std::span<const std::byte> bytes)
  {
    guard.check_and_register_output(reinterpret_cast<uintptr_t>(bytes.data()),
                                    bytes.size());
  }
};

} // namespace hogd
