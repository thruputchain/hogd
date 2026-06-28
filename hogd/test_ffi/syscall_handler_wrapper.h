#pragma once

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
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
  SyscallHandlerContext(std::shared_ptr<MockRuntimeContext> context,
                        SignedTransaction const &stx, DBContext &db);

  SyscallHandlerContext(std::shared_ptr<MockRuntimeContext> context,
                        DBContext &db, AuthClaim const &auth_claim,
                        SHA256Hash const &source_tx_hash);

  ~SyscallHandlerContext()
  {
    // The transaction must have been explicitly committed or aborted before the
    // handler is destroyed. We deliberately do NOT abort here: the abort path
    // issues a DB RPC that can throw, and the destructor runs at an implicit
    // noexcept boundary, so a throwing abort would terminate the process
    // instead of surfacing a structured error (issue #2488). close_syscall_
    // handler() aborts any still-open transaction before deleting the handler,
    // so reaching this point uncommitted is a programming error.
    CHECK(transaction_committed_or_aborted_);
    CHECK(mock_runtime_stack_.empty());
    // Destroying a handler that is still published in this thread's TLS context
    // slot would leave current_context_ dangling, so the next pop_context()
    // would dereference freed storage (issue #2784). close_syscall_handler()
    // rejects that close up front; this CHECK turns any other path that reaches
    // destruction while still installed into a deterministic abort rather than
    // a latent use-after-free.
    CHECK(get_context() != this);
    // The open-handler guard is released by open_handler_guard_'s destructor
    // (declared before handler_, destroyed after it), not here -- see #2572.
  }

  SyscallHandlerContext(SyscallHandlerContext const &) = delete;
  SyscallHandlerContext &operator=(SystemCallHandler const &) = delete;
  SyscallHandlerContext(SyscallHandlerContext &&) = delete;
  SyscallHandlerContext &operator=(SystemCallHandler &&) = delete;

  // enable_tracing turns on syscall tracing for this root invocation. It is
  // applied inside root_invoke(), after the mocked-handler/re-invocation
  // guards, so that SystemCallHandler::enable_tracing()'s empty-frame-stack
  // assertion can never fire ahead of a structured rejection (issue #2601).
  ForeignTracedInvokeResult root_invoke(bool enable_tracing);

  TestFFIErrorCode start_mock_invocation_frame(Invocation const &invocation,
                                               InvokeFlagSet invoke_flags);
  ForeignTracedInvokeResult
  finish_mock_invocation_frame(InvokeErrorCode invoke_error_code);

  static SyscallHandlerContext *get_context() { return current_context_; }

  [[nodiscard]]
  bool push_context()
  {
    if (get_context() != nullptr) {
      // The exported pop/push_context() FFI entries are not noexcept, so a
      // std::bad_alloc from this guard diagnostic (error() formats and
      // allocates) would propagate across the C ABI. Keep it best-effort, and
      // report a logging failure via fputs (no allocation) rather than silently
      // dropping it, so the guard still returns false (issue #2559).
      try {
        error("push_context error: threadlocal SyscallHandlerContext already "
              "exists");
      } catch (std::exception const &e) {
        std::fputs("error logging push_context guard: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputc('\n', stderr);
      }
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

  // The DBContext this handler borrows. Exposed so close_syscall_handler() can
  // poison the DB lifecycle if the implicit abort fails (issue #2596).
  DBContext &borrowed_db() { return db_; }

  x23::Async<std::optional<TxEventLog>> get_tx_event_log()
  {
    return db_.get_tx_event_log(identifier_);
  }

  // Guard against destructor assetion -- don't close the handler before popping
  // all frames off of the transaction.
  //
  // This intentionally keys off mock_runtime_stack_ rather than the lower
  // invocation-frame stack (handler_.is_running()): a mock callback that throws
  // unwinds before finish_frame() pops the lower frame, so the invocation-frame
  // stack can legitimately retain a dangling frame on a handler that must still
  // close cleanly (#2470). mock_runtime_stack_ is RAII-managed, so it is the
  // signal for "frames the harness still owes a finish". The half-open-frame
  // window where the two stacks could diverge during open is closed by making
  // start_mock_invocation_frame() exception-atomic (#2507, #2560).
  [[nodiscard]] bool can_be_closed() const
  {
    return mock_runtime_stack_.empty();
  }

  void mark_committed_or_aborted() { transaction_committed_or_aborted_ = true; }

  [[nodiscard]] bool is_committed_or_aborted() const
  {
    return transaction_committed_or_aborted_;
  }

  // Set when finish_mock_invocation_frame() finalized the root frame but then
  // failed to hand the result buffers back to the caller (e.g. allocation
  // failure during FFI serialization). The finalized result was never returned
  // to the caller, so try_commit_transaction() must not commit it (issue
  // #2508).
  void mark_finish_result_handoff_failed()
  {
    finish_result_handoff_failed_ = true;
  }
  [[nodiscard]] bool finish_result_handoff_failed() const
  {
    return finish_result_handoff_failed_;
  }

  // root_invoke() installs this handler as the thread-local context for the
  // duration of the root call and owns that slot. The exported pop_context()
  // helper consults this so a mock contract callback cannot pop the root-owned
  // context out from under the running root invocation (issue #2510).
  void set_root_invoke_owns_context(bool owned)
  {
    root_invoke_owns_context_ = owned;
  }
  [[nodiscard]] bool root_invoke_owns_context() const
  {
    return root_invoke_owns_context_;
  }

private:
  // RAII for the DBContext open-handler guard. Declared as a member *before*
  // handler_ so it raises open_handler_count_ before handler_ borrows the DB's
  // snapshot and proxies in the member-init list. Previously the guard was
  // raised in the constructor body, i.e. after handler_ had already borrowed
  // DB state, leaving a window where a concurrent advance_new_block() saw a
  // zero count and replaced the snapshot the half-built handler referenced
  // (issue #2572). Released in the destructor (members destroyed in reverse
  // declaration order, so after handler_).
  class OpenHandlerGuard {
  public:
    explicit OpenHandlerGuard(DBContext &db) : db_(db)
    {
      db_.register_open_handler();
    }
    ~OpenHandlerGuard() { db_.unregister_open_handler(); }
    OpenHandlerGuard(OpenHandlerGuard const &) = delete;
    OpenHandlerGuard &operator=(OpenHandlerGuard const &) = delete;
    OpenHandlerGuard(OpenHandlerGuard &&) = delete;
    OpenHandlerGuard &operator=(OpenHandlerGuard &&) = delete;

  private:
    DBContext &db_;
  };

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

  // Raised before handler_ borrows DB state; see OpenHandlerGuard (#2572).
  OpenHandlerGuard open_handler_guard_;

  // Keep the MockRuntimeContext alive for the handler's whole lifetime: the
  // embedded SystemCallHandler stores a RuntimeContext& into it, so closing the
  // test FFI runtime-context handle must not free it out from under an open
  // handler (issue #2432). Declared before handler_ so it is destroyed after
  // it.
  std::shared_ptr<MockRuntimeContext> runtime_context_owner_;

  SystemCallHandler handler_;
  std::vector<Runtime *> mock_runtime_stack_;
  std::vector<std::unique_ptr<InvocationFrameTrace>>
      invocation_frame_trace_stack_;
  bool transaction_committed_or_aborted_ = false;
  bool is_mocked_handler_;
  bool finish_result_handoff_failed_ = false;
  bool root_invoke_owns_context_ = false;
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

  // A *value* argument slot (a by-value integral/enum, as opposed to a memory
  // offset) whose value does not fit a 32-bit slot could never have been
  // produced by the real wasm32 runtime, which delivers 32-bit value slots.
  // Such a value means the test harness leaked an out-of-range value into the
  // syscall -- an FFI usage error, surfaced here as an unrecoverable (FFI)
  // error rather than a contract-observable syscall fault. Offset slots (spans,
  // references, optional pointers) legitimately carry full 64-bit host pointers
  // and the all-ones null sentinel, so they are excluded from this check.
  const bool value_slot_overflow =
      [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/) -> bool {
    return (((std::integral<typename syscall_t::template arg_i<Is>> ||
              std::is_enum_v<typename syscall_t::template arg_i<Is>>) &&
             std::get<syscall_t::template offset_i<Is>()>(input_args) >
                 std::numeric_limits<uint32_t>::max()) ||
            ... || false);
  }(from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{}));
  if (value_slot_overflow) {
    return unrecoverable_error();
  }

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
