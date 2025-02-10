#pragma once

#include <cassert>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/mock_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

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
  return ForeignHostFnResult{.result_code = std::to_underlying(result.first),
                             .panic = result.second};
}

ForeignHostFnResult constexpr from_status(
    std::pair<HostFnResult, GasAmount> status)
{
  return from_status(status.first);
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
    auto msg = std::format("{} Input {}\n", syscall_no, x23::show(*input_arg));
    write(syscall_logfd, msg.data(), msg.size());
  }

  auto output_arg = read_arg<OutputArg>(output);
  if (!output_arg) {
    return unrecoverable_error();
  }

  auto status = mock_func_inner(*input_arg, *output_arg);

  if (!write_output_arg(*output_arg, output)) {
    return unrecoverable_error();
  }

  return status;
}

} // namespace hogd
