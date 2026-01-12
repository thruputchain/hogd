#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Wrapper around the SyscallHandler.
// Stores the SyscallHandler in a threadlocal variable (which means
// that the test harness does not need to maintain that context externally).
class SyscallHandlerContext {
public:
  SyscallHandlerContext(MockRuntimeContext *context,
                        SignedTransaction const &stx, DBContext &db);

  ~SyscallHandlerContext()
  {
    if (!transaction_committed_or_aborted_) {
      warn("SyscallHandlerContext closed without committing: aborting "
           "transaction");
      auto res = handler_.abort_transaction();
      if (!res.has_value()) {
        try {
          warn("Failed to abort transaction: {}", res.error());
        } catch (std::exception const &e) {
          panic("failed to print error message: {}", e.what());
        }
      }
    }
  }

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

    assert(mock_runtime_stack_.empty());
    current_context_ = this;
    return true;
  }

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void pop_context()
  {
    assert(get_context() == this);
    current_context_ = nullptr;
    assert(mock_runtime_stack_.empty());
  }

  void push_mock_runtime(MockRuntime *runtime)
  {
    assert(get_context() == this);
    mock_runtime_stack_.push_back(runtime);
  }

  void pop_mock_runtime()
  {
    assert(get_context() == this);
    assert(!mock_runtime_stack_.empty());
    mock_runtime_stack_.pop_back();
  }

  MockRuntime *get_mock_runtime()
  {
    assert(get_context() == this);
    assert(!mock_runtime_stack_.empty());
    return mock_runtime_stack_.back();
  }

  SystemCallHandler &to_handler() { return handler_; }

  std::optional<TxEventLog> get_tx_event_log()
  {
    return db_.get_tx_event_log(identifier_);
  }

  void mark_committed_or_aborted() { transaction_committed_or_aborted_ = true; }

private:
  inline static thread_local SyscallHandlerContext *current_context_ = nullptr;

  DBContext::EphemeralTxIdentifier identifier_;
  DBContext &db_;

  SystemCallHandler handler_;
  std::vector<MockRuntime *> mock_runtime_stack_;
  bool transaction_committed_or_aborted_ = false;
};

constexpr ForeignHostFnResult
recoverable_error(SyscallErrorCode code)
{
  return ForeignHostFnResult{.result_code = std::to_underlying(code),
                             .panic = std::to_underlying(
                                 HostFnErrorCode::kHostNoneOrRecoverable)};
}

constexpr ForeignHostFnResult
unrecoverable_error()
{
  return ForeignHostFnResult{.result_code = 0,
                             .panic = std::to_underlying(
                                 HostFnErrorCode::kHostUnrecoverable)};
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

// Parse the arguments of a syscall and forward them to run_mock_func_inner
template<typename SyscallMeta>
ForeignHostFnResult
mock_syscall_parser(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
                    uint64_t arg5, uint64_t arg6, uint64_t arg7)
{
  auto input_args = std::tuple{arg1, arg2, arg3, arg4, arg5, arg6, arg7};
  using syscall_t =
      SyscallTypes<std::remove_const_t<decltype(SyscallMeta::kInnerFunc)>>;

  auto *handler = SyscallHandlerContext::get_context();
  if (!handler) {
    return unrecoverable_error();
  }

  auto *mock_runtime = handler->get_mock_runtime();
  assert(mock_runtime != nullptr);

  // Again, parameter packs as structured bindings gets rid of the annoying
  // lambda context
  return [&]<size_t... Is>(
             TupleIndexSequence<Is...> /*unused*/) -> ForeignHostFnResult {
    // Attempt to load syscall arguments from regular machine, not contract)
    // memory.
    auto parsed_args = std::tuple{
        parse_mock<syscall_t::template offset_i<Is>(),
                   typename syscall_t::template arg_i<Is>>(input_args)...};

    auto inner_res =
        wrap_after_parse<SyscallMeta>(handler->to_handler(),
                                      WrappedGasAmount{
                                          mock_runtime->get_available_gas()},
                                      std::move(parsed_args));

    auto gas_consumed = inner_res.second;
    if (!mock_runtime->consume_gas(gas_consumed)) {
      return {.result_code = 0,
              .panic = std::to_underlying(HostFnErrorCode::kHostOutOfGas)};
    }

    if (!SyscallMeta::kCanReturn && inner_res.first.has_value()) {
      return {.result_code = 0,
              .panic = std::to_underlying(HostFnErrorCode::kHostExit)};
    }

    if constexpr (is_debug_build()) {
      // In debug builds, verify that the syscall only returns documented error
      // codes. This helps catch cases where a syscall returns an error that
      // isn't documented in syscalls.yaml.
      if (inner_res.first.has_value() &&
          !is_allowed_error_code<SyscallMeta>(inner_res.first.value())) {
        panic("Syscall {} returned undocumented error code {}",
              SyscallMeta::kSyscallNo, x23::show(inner_res.first.value()));
      }
    }

    return from_status(inner_res);
  }(from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{}));
}

} // namespace hogd
