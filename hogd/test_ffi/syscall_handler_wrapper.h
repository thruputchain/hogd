#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
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
#include "hogd/hogexec/runtime/syscall_arg_params.h"
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
template<SyscallNo Syscall, typename InputArg, typename OutputArg,
         auto InnerSyscallFunc>
ForeignHostFnResult
mock_syscall_parser(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
                    uint64_t arg5, uint64_t arg6)
{
  auto input_args = std::tuple{arg1, arg2, arg3, arg4, arg5, arg6};
  using syscall_t = SyscallTypes<decltype(InnerSyscallFunc)>;

  // Again, parameter packs as structured bindings gets rid of the annoying
  // lambda context
  return [&]<size_t... Is>(
             TupleIndexSequence<Is...> /*unused*/) -> ForeignHostFnResult {
    // Attempt to load syscall arguments from regular machine, not contract)
    // memory.
    auto parsed_args = std::tuple{
        parse_mock<syscall_t::template offset_i<Is>(),
                   typename syscall_t::template arg_i<Is>>(input_args)...};

    // Check whether any loads failed
    if (!((SyscallArgParams<typename syscall_t::template arg_i<Is>>::validate(
               std::get<Is>(parsed_args)) &&
           ...))) {
      return recoverable_error(SyscallErrorCode::kFault);
    }

    // Once the syscall inputs are parsed, we can print them in a formatted log
    // message.
    if (syscall_logfd != -1) {
      auto log_string =
          x23::show(make_syscall_input_struct<InputArg>(std::tuple{
              SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                  std::get<Is>(parsed_args))...}));
      write(syscall_logfd, log_string.c_str(), log_string.size());
    }

    // With parameter packs as structured bindings, the annoying if constexpr
    // disappears, and the following becomes
    //   auto [...extra_args] = syscall_t::extra_args(handler, etc);
    //   auto res = f(extra_args...,
    //           SyscallArgParams<typename syscall_t::template
    //           arg_i<Is>>::deref(
    //               parsed_args...[Is])...);

    auto *handler = SyscallHandlerContext::get_context();
    if (!handler) {
      return unrecoverable_error();
    }

    auto extra_args =
        syscall_t::extra_args(handler->to_handler(),
                              WrappedGasAmount{
                                  std::numeric_limits<uint64_t>::max()});

    // Invoke the inner syscall
    auto res =
        [&]<size_t... ExtraArgs>(std::index_sequence<ExtraArgs...> /*unused*/) {
          return InnerSyscallFunc(
              std::get<ExtraArgs>(extra_args)...,
              SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                  std::get<Is>(parsed_args))...);
        }(std::make_index_sequence<std::tuple_size_v<decltype(extra_args)>>{});

    // Print the output of the syscall.  The outputs in parsed_args should only
    // be written to if the syscall succeeds.
    if (syscall_logfd != -1) {
      auto [host_fn_code, syscall_code] = convert_return_t(res.first);
      std::string log_string;
      if (host_fn_code != HostFnErrorCode::kNoneOrRecoverable ||
          syscall_code != 0) {
        log_string = std::format("Syscall Error: {} {}",
                                 HostFnErrorCategory{}.message(
                                     std::to_underlying(host_fn_code)),
                                 x23::show(SyscallErrorCode(syscall_code)));
      }
      else {
        log_string = x23::show(
            make_syscall_output_struct<InputArg, OutputArg>(std::tuple{
                SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                    std::get<Is>(parsed_args))...}));
      }
      write(syscall_logfd, log_string.c_str(), log_string.size());
    }

    return from_status(res.first);
  }(from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{}));
}

/*


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
} */

} // namespace hogd
