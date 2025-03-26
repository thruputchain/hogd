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
#include <print>
#include <span>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/mock_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

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

  ~SyscallHandlerContext() { per_tx_db_proxy_.do_explicit_cleanup(); }

private:
  inline static thread_local SyscallHandlerContext *current_context_ = nullptr;

  PerTransactionDatabaseProxy per_tx_db_proxy_;
  SystemCallHandler handler_;
};

template<typename Arg>
std::optional<Arg>
read_arg(std::span<const uint8_t> input)
{
  try {
    Arg out;
    x23::deserialize(&out, input);
    return out;
  } catch (x23::XdrBadInput &e) {
    std::println("xdr deserialize error! %s", e.what());
    return std::nullopt;
  }
}

template<typename Arg>
bool
write_output_arg(Arg const &arg, std::span<uint8_t> output)
{
  try {
    auto res = x23::serialize(arg);
    if (res.size() != output.size()) {
      std::println("Output serialize len mismatch!");
      return false;
    }
    std::memcpy(output.data(), res.data(), res.size());
    return true;
  } catch (std::exception &e) {
    std::println("xdr serialize error! %s", e.what());
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

auto
input_xdr_to_input(auto arg)
{
  using T = std::remove_cvref_t<decltype(arg)>;

  if constexpr (x23::is_bytevec<T>) {
    return std::as_bytes(std::span(arg));
  }
  else if constexpr (requires { arg.get(); }) {
    return arg.get();
  }
  else if constexpr (std::same_as<int128_xdr, T>) {
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

template<typename T, typename U>
decltype(auto)
output_xdr_to_output(T &arg, U &local_arg_alloc)
{
  static_assert(std::same_as<U, decltype(cleanup_type(std::declval<T>()))>);

  if constexpr (std::same_as<T, int128_xdr>) {
    return local_arg_alloc;
  }
  else if constexpr (x23::is_bytevec<T>) {
    return std::as_writable_bytes(std::span(arg));
  }
  else {
    return (arg);
  }
}

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

template<typename T>
constexpr static auto
cleanup_tuple(T const &arg)
{
  return [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    return std::tuple{cleanup_type(get<Is>(arg))...};
  }(std::make_index_sequence<std::tuple_size_v<T>>{});
}

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
  // xdr. This is annoying.
  cleanup_args_t local_args_alloc{};

  struct NullRuntime : public Runtime {
    NullRuntime() : Runtime(std::make_unique<HostCallContext>()) {}
    uint64_t get_available_gas() const noexcept override
    {
      return std::numeric_limits<GasAmount>::max();
    }
    void set_available_gas(uint64_t /*unused*/) noexcept override
    {
      std::terminate();
    }

    std::span<std::byte> get_memory() noexcept override { std::terminate(); }
    std::span<const std::byte> get_memory() const noexcept override
    {
      std::terminate();
    }

    ~NullRuntime() = default;

    InvokeStatus<int32_t>
    invoke_internal(std::string const & /*unused*/) override
    {
      std::terminate();
    }
  };

  NullRuntime null_runtime;

  auto extras_pack = syscall_t::extras(handler->to_handler(), null_runtime);

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
      run_mock_func_inner<InputArg, OutputArg, mock_func_inner>(
          *input_arg, *output_arg); // mock_func_inner(*input_arg, *output_arg);

  if (status.panic != std::to_underlying(HostFnErrorCode::kNoneOrRecoverable)) {
    if (syscall_logfd != -1) {
      auto panic_msg = std::format("GUEST PANIC {}", status.panic);
      if (status.panic == std::to_underlying(HostFnErrorCode::kExit)) {
        panic_msg = "GUEST EXIT";
      }
      else if (status.panic == std::to_underlying(HostFnErrorCode::kReturn)) {
        panic_msg = "GUEST RETURN";
      }
      auto msg = std::format("{} {}\n", syscall_no, panic_msg);
      write(syscall_logfd, msg.data(), msg.size());
    }

    return status;
  }

  if (status.result_code != std::to_underlying(SyscallErrorCode::kNone)) {
    if (syscall_logfd != -1) {
      auto msg =
          std::format("{} return error {}\n", syscall_no, status.result_code);
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
