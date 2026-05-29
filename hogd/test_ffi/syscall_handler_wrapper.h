#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/alias_guard.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// Wrapper around the SyscallHandler.
// Stores the SyscallHandler in a threadlocal variable (which means
// that the test harness does not need to maintain that context externally).
class SyscallHandlerContext {
public:
  SyscallHandlerContext(MockRuntimeContext *context,
                        SignedTransaction const &stx, DBContext &db);

  SyscallHandlerContext(MockRuntimeContext *context, DBContext &db,
                        AuthClaim const &auth_claim,
                        SHA256Hash const &source_tx_hash);

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
    CHECK(mock_runtime_stack_.empty());
  }

  SyscallHandlerContext(SyscallHandlerContext const &) = delete;
  SyscallHandlerContext &operator=(SystemCallHandler const &) = delete;
  SyscallHandlerContext(SyscallHandlerContext &&) = delete;
  SyscallHandlerContext &operator=(SystemCallHandler &&) = delete;

  ForeignTracedInvokeResult root_invoke();

  TestFFIErrorCode start_mock_invocation_frame(Invocation const &invocation,
                                               InvokeFlagSet invoke_flags);
  ForeignTracedInvokeResult
  finish_mock_invocation_frame(InvokeErrorCode invoke_error_code);

  static SyscallHandlerContext *get_context() { return current_context_; }

  [[nodiscard]]
  bool push_context()
  {
    if (get_context() != nullptr) {
      error("push_context error: threadlocal SyscallHandlerContext already "
            "exists");
      return false;
    }

    current_context_ = this;
    return true;
  }

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void pop_context()
  {
    CHECK(get_context() == this);
    current_context_ = nullptr;
  }

  void push_mock_runtime(Runtime *runtime)
  {
    mock_runtime_stack_.push_back(runtime);
  }

  Runtime *pop_mock_runtime()
  {
    assert(!mock_runtime_stack_.empty());
    auto *runtime = mock_runtime_stack_.back();
    mock_runtime_stack_.pop_back();
    return runtime;
  }

  Runtime *get_mock_runtime()
  {
    if (mock_runtime_stack_.empty()) {
      return nullptr;
    }
    return mock_runtime_stack_.back();
  }

  SystemCallHandler &to_handler() { return handler_; }

  x23::Async<std::optional<TxEventLog>> get_tx_event_log()
  {
    return db_.get_tx_event_log(identifier_);
  }

  // Guard against destructor assetion -- don't close the handler before popping
  // all frames off of the transaction.
  [[nodiscard]] bool can_be_closed() const
  {
    return mock_runtime_stack_.empty();
  }

  void mark_committed_or_aborted() { transaction_committed_or_aborted_ = true; }

private:
  constexpr static auto kNullBuffer = FFIDataBuffer{.data = nullptr, .len = 0};
  constexpr static auto kMakeFFIError =
      [](TestFFIErrorCode code) -> ForeignTracedInvokeResult {
    return {
        .serialized_output = kNullBuffer,
        .gas_consumed = 0,
        .serialized_trace = kNullBuffer,
        .serialized_events = kNullBuffer,
        .ffi_error_code = code,
    };
  };

  inline static thread_local SyscallHandlerContext *current_context_ = nullptr;

  DBContext::EphemeralTxIdentifier identifier_;
  DBContext &db_;

  SystemCallHandler handler_;
  std::vector<Runtime *> mock_runtime_stack_;
  std::vector<std::unique_ptr<InvocationFrameTrace>>
      invocation_frame_trace_stack_;
  bool transaction_committed_or_aborted_ = false;
  bool is_mocked_handler_;
};

constexpr ForeignHostFnResult
recoverable_error(SyscallErrorCode code)
{
  return ForeignHostFnResult{.result_code = std::to_underlying(code),
                             .panic = std::to_underlying(
                                 HostFnErrorCode::NoneOrRecoverable)};
}

constexpr ForeignHostFnResult
unrecoverable_error()
{
  return ForeignHostFnResult{.result_code = 0,
                             .panic = std::to_underlying(
                                 HostFnErrorCode::Unrecoverable)};
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

// Parse the arguments of a syscall and forward them to run_mock_func_inner
template<typename SyscallMeta>
ForeignHostFnResult
mock_syscall_parser(SyscallNo syscall_no, uint64_t arg1, uint64_t arg2,
                    uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6,
                    uint64_t arg7)
{
  auto input_args = std::tuple{arg1, arg2, arg3, arg4, arg5, arg6, arg7};
  using syscall_t =
      SyscallTypes<std::remove_const_t<decltype(SyscallMeta::kInnerFunc)>>;

  auto *handler = SyscallHandlerContext::get_context();
  if (!handler) {
    error("Unrecoverable: mock_syscall_parser got null "
          "SyscallHandlerContext for syscall {}",
          std::to_underlying(syscall_no));
    return unrecoverable_error();
  }

  auto *mock_runtime = handler->get_mock_runtime();
  CHECK(mock_runtime != nullptr);
  auto *gas_runtime = dynamic_cast<MockGasRuntime *>(mock_runtime);
  CHECK(gas_runtime != nullptr);

  // Again, parameter packs as structured bindings gets rid of the annoying
  // lambda context
  return [&]<size_t... Is>(
             TupleIndexSequence<Is...> /*unused*/) -> ForeignHostFnResult {
    AliasGuard guard;
    // Attempt to load syscall arguments from regular machine, not contract)
    // memory.
    auto parsed_args = std::tuple{
        parse_mock<syscall_t::template offset_i<Is>(),
                   typename syscall_t::template arg_i<Is>>(guard,
                                                           input_args)...};

    auto [inner_res, gas] =
        wrap_after_parse<SyscallMeta>(guard, handler->to_handler(),
                                      gas_runtime->gas_remaining(), syscall_no,
                                      std::move(parsed_args));
    gas_runtime->consume_gas(gas);

    return from_status(inner_res);
  }(from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{}));
}

} // namespace hogd
