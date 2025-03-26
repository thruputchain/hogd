#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

// Wrapper around the SyscallHandler.
// Stores the SyscallHandler in a threadlocal variable (which means
// that the test harness does not need to maintain that context externally).
class SyscallHandlerContext {
public:
  SyscallHandlerContext(std::shared_ptr<MockRuntimeContext> context,
                        SignedTransaction const &stx, DBContext &db);

  SyscallHandlerContext(SyscallHandlerContext const &) = delete;
  SyscallHandlerContext &operator=(SystemCallHandler const &) = delete;
  SyscallHandlerContext(SyscallHandlerContext &&) = delete;
  SyscallHandlerContext &operator=(SystemCallHandler &&) = delete;

  ForeignRootInvokeResult root_invoke();

  static SyscallHandlerContext *get_context() { return current_context_; }

  [[nodiscard]]
  bool push_context()
  {
    if (get_context() != nullptr) {
      return false;
    }

    current_context_ = this;
    return true;
  }

  void pop_context()
  {
    assert(get_context() == this);
    current_context_ = nullptr;
  }

  SystemCallHandler &to_handler() { return handler_; }

private:
  inline static thread_local SyscallHandlerContext *current_context_ = nullptr;

  SystemCallHandler handler_;
};

// Arguments to the mock syscall are first
// deserialized from raw XDR.
// The structs are those generated in syscall_args.x
// ({Syscall}_input and {Syscall}_output).
template<typename Arg>
std::optional<Arg>
read_arg(std::span<const uint8_t> input)
{
  try {
    Arg out;
    x23::deserialize(&out, input);
    return out;
  } catch (x23::XdrBadInput &e) {
    if (syscall_logfd != -1) {
      std::string message =
          std::string("Syscall input xdr deserialize error") + e.what();
      write(syscall_logfd, message.c_str(), message.size());
    }
    return std::nullopt;
  }
}

// Serializes the output of a syscall to a memory span
// provided by the caller.
template<typename Arg>
bool
write_output_arg(Arg const &arg, std::span<uint8_t> output)
{
  try {
    auto res = x23::serialize(arg);
    if (res.size() != output.size()) {
      if (syscall_logfd != -1) {
        std::string message = "Syscall output serialize len mismatch";
        write(syscall_logfd, message.c_str(), message.size());
      }
      return false;
    }
    std::memcpy(output.data(), res.data(), res.size());
    return true;
  } catch (std::exception &e) {
    if (syscall_logfd != -1) {
      std::string message =
          std::string("Syscall output xdr serialize error!") + e.what();
      write(syscall_logfd, message.c_str(), message.size());
    }
    return false;
  }
}

constexpr ForeignHostFnResult
recoverable_error(SyscallErrorCode code)
{
  return ForeignHostFnResult{.result_code = std::to_underlying(code),
                             .panic = std::to_underlying(
                                 HostFnErrorCode::kNoneOrRecoverable)};
}

constexpr ForeignHostFnResult
unrecoverable_error()
{
  return ForeignHostFnResult{.result_code = 0,
                             .panic = std::to_underlying(
                                 HostFnErrorCode::kUnrecoverable)};
}

// Translate various types of syscall return types into FFI-safe return type
ForeignHostFnResult constexpr from_status(HostFnStatus<SyscallErrorCode> status)
{
  if (status) {
    return recoverable_error(*status);
  }
  return ForeignHostFnResult{.result_code = 0,
                             .panic =
                                 std::to_underlying(status.error().code())};
}

ForeignHostFnResult constexpr from_status(
    std::pair<HostFnStatus<SyscallErrorCode>, GasAmount> status)
{
  return from_status(status.first);
}

ForeignHostFnResult constexpr from_status(HostFnResult result)
{
  return ForeignHostFnResult{.result_code = result.second,
                             .panic = std::to_underlying(result.first)};
}

ForeignHostFnResult constexpr from_status(
    std::pair<HostFnResult, GasAmount> status)
{
  return from_status(status.first);
}

// Turn the input type (the xdr type) into the corresponding
// type accepted by the syscall.
auto
input_xdr_to_input(auto arg)
{
  using T = std::remove_cvref_t<decltype(arg)>;

  if constexpr (x23::is_bytevec<T>) {
    // span types
    return std::as_bytes(std::span(arg));
  }
  else if constexpr (requires { arg.get(); }) {
    // pointer types
    return arg.get();
  }
  else if constexpr (std::same_as<int128_xdr, T>) {
    // int128 requires special handling
    return from_xdr(arg);
  }
  else {
    return arg;
  }
}

struct NoCleanupRequired {};

constexpr static auto
cleanup_type(auto arg)
{
  using T = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::same_as<T, int128_xdr>) {
    return int128_t{};
  }
  else {
    return NoCleanupRequired{};
  }
}

// Turn the output type (the xdr type, as a reference) into the corresponding
// type accepted by the syscall.
// Note that not all types can be converted in this way;
// the main exception is int128.
// Those types require a special cleanup phase
// after the syscall.
// For those types, we allocate an instance of the non-xdr type first,
// pass a reference to that into the syscall, and then later copy that object
// into the output xdr.
template<typename T, typename U>
decltype(auto)
output_xdr_to_output(T &arg, U &local_arg_alloc)
{
  static_assert(std::same_as<U, decltype(cleanup_type(std::declval<T>()))>);

  if constexpr (std::same_as<T, int128_xdr>) {
    return local_arg_alloc;
  }
  else if constexpr (x23::is_bytevec<T>) {
    // bytevector -> span
    return std::as_writable_bytes(std::span(arg));
  }
  else {
    // references -> references
    return (arg);
  }
}

// Types that require post-syscall cleanup to add to the output xdr
void
cleanup_local_output(auto &arg, auto const &local_arg_alloc)
{
  using T = std::remove_cvref_t<decltype(arg)>;
  using T_local = std::remove_cvref_t<decltype(local_arg_alloc)>;
  if constexpr (std::same_as<T, int128_xdr>) {
    static_assert(std::same_as<T_local, int128_t>);
    arg = to_xdr(local_arg_alloc);
  }
}

// Create a tuple of objects that need special handling.
// At the moment, that only type is int128.
template<typename T>
constexpr static auto
cleanup_tuple(T const &arg)
{
  return [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    return std::tuple{cleanup_type(get<Is>(arg))...};
  }(std::make_index_sequence<std::tuple_size_v<T>>{});
}

// Wrapper around the actual inner syscall, after transforming the xdr arguments
// into actual arguments and adding any relevant context.
template<typename InputArg, typename OutputArg, auto mock_func_inner>
ForeignHostFnResult
run_mock_func_inner(InputArg const &input, OutputArg &output)
{
  auto *handler = SyscallHandlerContext::get_context();
  if (!handler) {
    return unrecoverable_error();
  }

  using syscall_t = SyscallTypes<decltype(mock_func_inner)>;
  using cleanup_args_t = decltype(cleanup_tuple(output));

  // One type (int128_xdr) needs to get converted before being put back into the
  // xdr.
  cleanup_args_t local_args_alloc{};

  auto extras_pack =
      syscall_t::extra_args(handler->to_handler(),
                            WrappedGasAmount{
                                std::numeric_limits<uint64_t>::max()});

  return [&]<size_t... Extras>(std::index_sequence<Extras...> /*unused*/) {
    return [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
      return [&]<size_t... Os>(std::index_sequence<Os...> /*unused*/) {
        auto res = from_status(mock_func_inner(
            get<Extras>(extras_pack)..., input_xdr_to_input(get<Is>(input))...,
            output_xdr_to_output(get<Os>(output),
                                 get<Os>(local_args_alloc))...));

        (cleanup_local_output(get<Os>(output), get<Os>(local_args_alloc)), ...);
        return res;
      }(std::make_index_sequence<std::tuple_size_v<OutputArg>>());
    }(std::make_index_sequence<std::tuple_size_v<InputArg>>());
  }(std::make_index_sequence<std::tuple_size_v<decltype(extras_pack)>>());
}

// Parse the arguments of a syscall and forward them to run_mock_func_inner
template<typename InputArg, typename OutputArg, auto mock_func_inner>
ForeignHostFnResult
mock_syscall_parser(SyscallNo syscall_no, std::span<const uint8_t> input,
                    std::span<uint8_t> output)
{
  auto input_arg = read_arg<InputArg>(input);
  if (!input_arg) {
    return unrecoverable_error();
  }

  if (syscall_logfd != -1) {
    auto msg = std::format("{} call {}\n", syscall_no, x23::show(*input_arg));
    write(syscall_logfd, msg.data(), msg.size());
  }

  auto output_arg = read_arg<OutputArg>(output);
  if (!output_arg) {
    return unrecoverable_error();
  }

  ForeignHostFnResult status =
      run_mock_func_inner<InputArg, OutputArg, mock_func_inner>(*input_arg,
                                                                *output_arg);

  if (status.panic != std::to_underlying(HostFnErrorCode::kNoneOrRecoverable)) {
    if (syscall_logfd != -1) {
      auto panic_msg = std::format("GUEST PANIC {}", status.panic);
      if (status.panic == std::to_underlying(HostFnErrorCode::kExit)) {
        panic_msg = "GUEST EXIT";
      }
      else if (status.panic == std::to_underlying(HostFnErrorCode::kReturn)) {
        panic_msg = "GUEST RETURN";
      }
      auto msg = std::format("{} {}\n", x23::show(syscall_no), panic_msg);
      write(syscall_logfd, msg.data(), msg.size());
    }

    return status;
  }

  if (status.result_code != std::to_underlying(SyscallErrorCode::kNone)) {
    if (syscall_logfd != -1) {
      auto msg = std::format("{} return error {}\n", x23::show(syscall_no),
                             status.result_code);
      write(syscall_logfd, msg.data(), msg.size());
    }

    return status;
  }

  if (!write_output_arg(*output_arg, output)) {
    return unrecoverable_error();
  }

  if (syscall_logfd != -1) {
    auto msg =
        std::format("{} return {}\n", syscall_no, x23::show(*output_arg));
    write(syscall_logfd, msg.data(), msg.size());
  }

  return status;
}

} // namespace hogd
