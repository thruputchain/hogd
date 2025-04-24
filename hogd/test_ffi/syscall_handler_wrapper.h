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

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
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

} // namespace hogd
