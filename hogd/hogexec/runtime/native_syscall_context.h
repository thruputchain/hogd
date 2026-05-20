#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/alias_guard.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_meta.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "x23/vec.h"

namespace hogd {

// Routes native contract syscalls through wrap_after_parse, so they get
// read-only enforcement, tracing, and gas accounting -- just like wasm
// contracts.
class NativeSyscallContext {
public:
  NativeSyscallContext(SystemCallHandler &h, GasAmount gas_limit)
    : handler_(h), initial_gas_amount_(gas_limit)
  {}

  template<SyscallNo Syscall, typename... Args>
  HostFnStatus<SyscallErrorCode> syscall(Args &&...args)
  {
    using Meta = SyscallMeta<Syscall>;
    using syscall_t =
        SyscallTypes<std::remove_const_t<decltype(Meta::kInnerFunc)>>;
    static_assert(sizeof...(Args) == syscall_t::kNumArgs);

    AliasGuard guard;

    auto parsed = [&]<size_t... Is>(std::index_sequence<Is...>) {
      return std::tuple{to_parsed<typename syscall_t::template arg_i<Is>>(
          std::forward<Args>(args))...};
    }(std::make_index_sequence<sizeof...(Args)>{});

    // std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
    auto [res, gas_consumed] =
        wrap_after_parse<Meta>(guard, handler_, gas_remaining(),
                               Meta::kSyscallNo, parsed);

    consume_gas(gas_consumed);
    return res;
  }

  SystemCallHandler &handler() { return handler_; }

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

  bool consume_gas(GasAmount amount)
  {
    auto [sum, of] = overflowing_add(amount, gas_consumed_);
    std::println("consume_gas: {} + {} = {}", gas_consumed_, amount, sum);

    if (of) {
      gas_consumed_ = std::numeric_limits<GasAmount>::max();
    }
    else {
      gas_consumed_ = sum;
    }

    std::println("Gas consumed: {}, initial_gas_amount_: {}", gas_consumed_,
                 initial_gas_amount_);

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
  template<typename ArgParam, typename Arg> static auto to_parsed(Arg &&arg)
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
    else if constexpr (x23::is_bytespan<ArgParam>) {
      using Span = std::remove_cvref_t<ArgParam>;
      if constexpr (std::is_const_v<typename Span::element_type>) {
        return std::optional{std::as_bytes(std::span(arg))};
      }
      else {
        return std::optional{std::as_writable_bytes(std::span(arg))};
      }
    }
    else {
      // Value types, input refs, spans: wrap in optional
      return std::optional{std::forward<Arg>(arg)};
    }
  }
};

} // namespace hogd
