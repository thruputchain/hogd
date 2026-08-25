#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/alias_guard.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
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
    // Destroying a handler that is still published on this thread's context
    // stack would leave a dangling entry, so the next pop_context() would
    // dereference freed storage (issue #2784). Scanning rather than checking
    // just the top: with a stack an entry can be buried under a nested guest
    // call, and that is equally fatal. close_syscall_handler()
    // rejects that close up front; this CHECK turns any other path that reaches
    // destruction while still installed into a deterministic abort rather than
    // a latent use-after-free.
    CHECK(std::ranges::find(context_stack_, this) == context_stack_.end());
    // The open-handler guard is released by open_handler_guard_'s destructor
    // (declared before handler_, destroyed after it), not here -- see #2572.
  }

  SyscallHandlerContext(SyscallHandlerContext const &) = delete;
  SyscallHandlerContext &operator=(SystemCallHandler const &) = delete;
  SyscallHandlerContext(SyscallHandlerContext &&) = delete;
  SyscallHandlerContext &operator=(SystemCallHandler &&) = delete;

  // enable_tracing turns on syscall tracing for this root invocation. It is
  // applied inside root_invoke(), after push_context() has accepted the call,
  // so that SystemCallHandler::enable_tracing()'s empty-frame-stack assertion
  // can never fire ahead of a structured rejection -- neither for a mocked
  // handler with a frame open (issue #2601) nor for a reentrant call from a
  // mock contract callback while the outer root frame is active (issue #2705).
  ForeignTracedInvokeResult root_invoke(bool enable_tracing);

  TestFFIErrorCode start_mock_invocation_frame(Invocation const &invocation,
                                               InvokeFlagSet invoke_flags);
  ForeignTracedInvokeResult
  finish_mock_invocation_frame(InvokeErrorCode invoke_error_code);

  // Recovers the context owning a handler. MockRuntimeContext::make_new_runtime
  // is handed the SystemCallHandler* hog_exec is running and has to reach the
  // harness state around it. It cannot ask the thread: a database read inside
  // hog_exec suspends the transaction and resumes it on whichever thread took
  // the reply, not the one that entered the FFI call.
  //
  // Every handler a MockRuntimeContext sees is the TestSystemCallHandler member
  // of some SyscallHandlerContext, so this is an ordinary base-to-derived
  // conversion -- defined, with no assumption about layout.
  //
  // kHandlerMagic still rejects a handler that is not one of ours. The
  // downcast itself cannot detect that (nor could the arithmetic this
  // replaced): nothing in the tree builds a bare SystemCallHandler and hands
  // it to a MockRuntimeContext, and if that changes this aborts rather than
  // returning a reference to whatever the bytes happen to hold.
  static SyscallHandlerContext *from_handler(SystemCallHandler *handler)
  {
    CHECK(handler != nullptr);
    auto *test_handler = static_cast<TestSystemCallHandler *>(handler);
    CHECK(test_handler->magic == kHandlerMagic);
    return &test_handler->owner;
  }

  static SyscallHandlerContext *get_context()
  {
    return context_stack_.empty() ? nullptr : context_stack_.back();
  }

  // Publishes a context on the calling thread for as long as it is alive.
  // MockRuntime::invoke_internal -- the one place control crosses back from
  // hog_exec into the harness -- installs the context its runtime was built
  // with for the duration of the guest call, rather than relying on the thread
  // already holding it. It cannot: a database read inside hog_exec suspends the
  // transaction and resumes it on whichever thread took the reply, not the one
  // that entered the FFI call.
  class ScopedContext {
  public:
    explicit ScopedContext(SyscallHandlerContext *ctx) noexcept : pushed_(ctx)
    {
      try {
        context_stack_.push_back(ctx);
      } catch (std::exception const &e) {
        std::fputs("ScopedContext: cannot grow the context stack: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputc('\n', stderr);
        throw; // noexcept: terminates here, with the reason already reported
      }
    }
    ~ScopedContext()
    {
      CHECK(!context_stack_.empty() && context_stack_.back() == pushed_);
      context_stack_.pop_back();
    }

    ScopedContext(ScopedContext const &) = delete;
    ScopedContext &operator=(ScopedContext const &) = delete;
    ScopedContext(ScopedContext &&) = delete;
    ScopedContext &operator=(ScopedContext &&) = delete;

  private:
    SyscallHandlerContext *pushed_;
  };

  // noexcept because this is reachable through the exported C entry point,
  // where a throw would unwind across the ABI into Rust. Growing the stack can
  // fail; making that a deterministic terminate at the throw point is the
  // defined behavior, and the diagnostic below is allocation-free so the reason
  // still lands (same reasoning as the guard logging, issue #2559).
  [[nodiscard]]
  bool push_context() noexcept
  {
    if (get_context() != nullptr) {
      // error() formats and allocates, so this diagnostic can throw. That must
      // not escape: this function is noexcept and has a structured rejection to
      // report, which a terminate would throw away. Keep it best-effort and
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

    try {
      context_stack_.push_back(this);
    } catch (std::exception const &e) {
      std::fputs("push_context: cannot grow the context stack: ", stderr);
      std::fputs(e.what(), stderr);
      std::fputc('\n', stderr);
      throw; // noexcept: terminates here, with the reason already reported
    }
    return true;
  }

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void pop_context() noexcept
  {
    CHECK(get_context() == this);
    context_stack_.pop_back();
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
  // invocation-frame stack (to_handler().is_running()): a mock callback that
  // throws unwinds before finish_frame() pops the lower frame, so the
  // invocation-frame stack can legitimately retain a dangling frame on a
  // handler that must still close cleanly (#2470). mock_runtime_stack_ is
  // RAII-managed, so it is the signal for "frames the harness still owes a
  // finish". The half-open-frame window where the two stacks could diverge
  // during open is closed by making start_mock_invocation_frame()
  // exception-atomic (#2507, #2560).
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

  // The contexts running on this thread, innermost last. A stack rather than a
  // single slot because a mock contract's guest call can nest: every frame
  // pushes on entry to MockRuntime::invoke_internal and pops on the way out, so
  // an inner frame does not have to clobber the outer one's entry.
  // Defined out of line in syscall_handler_wrapper.cc rather than inline here:
  // a vector needs dynamic initialization, so an inline thread_local emits a
  // TLS initialization routine in every translation unit that includes this
  // header, and macOS ld rejects those as duplicate symbols. Linux CI does not
  // see it -- test_bindings.yml is the only PR workflow that sets
  // ENABLE_TEST_FFI and it runs on Linux, so the macOS test_ffi build happens
  // only in release_test_ffi.yml.
  static thread_local std::vector<SyscallHandlerContext *> context_stack_;

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

  static constexpr std::uint64_t kHandlerMagic = 0x4d41524d4f54ULL; // MARMOT

  // The handler, carrying a reference back to the context that owns it.
  // Deriving keeps the backreference in the test-specific object: production
  // SystemCallHandler is untouched, and recovering the owner is a defined
  // downcast rather than pointer arithmetic over a layout we would have to
  // assume. SystemCallHandler has no virtual functions and is never deleted
  // through a base pointer here -- this is a by-value member -- so the absent
  // virtual destructor is not a hazard.
  //
  // Declared where the plain handler used to be, so the ordering the guard and
  // the runtime-context owner rely on (#2572, #2432) is unchanged.
  struct TestSystemCallHandler : SystemCallHandler {
    template<typename... Args>
    explicit TestSystemCallHandler(SyscallHandlerContext &owner_in,
                                   Args &&...args)
      : SystemCallHandler(std::forward<Args>(args)...), owner(owner_in)
    {}

    SyscallHandlerContext &owner;
    std::uint64_t magic = kHandlerMagic;
  };

  TestSystemCallHandler handler_;
  std::vector<Runtime *> mock_runtime_stack_;
  std::vector<std::unique_ptr<InvocationFrameTrace>>
      invocation_frame_trace_stack_;
  bool transaction_committed_or_aborted_ = false;
  bool is_mocked_handler_;
  bool finish_result_handoff_failed_ = false;
  bool root_invoke_owns_context_ = false;
  // Set immediately before SystemCallHandler::root_invoke() is entered.
  // root_invoke() consumes root_invocation_ early (before any frame starts), so
  // a pre-frame setup throw -- e.g. a runtime-resolution allocation failure --
  // leaves the handler live with last_frame_finished_ still false. A retry
  // would then re-enter the core CHECK(root_invocation_.has_value()); this flag
  // lets the wrapper reject the retry with a structured error instead (issue
  // #2643).
  bool root_invoke_attempted_ = false;
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

    // The mock entrypoint is an extern "C" ABI the Rust test harness calls
    // synchronously, so it blocks on the syscall result rather than awaiting
    // it.  The body really can suspend -- LocalMockDB is a hog_db on a
    // localhost socket, not an in-process fake -- but blocking is legal here
    // because the harness thread is never an x23 ThreadPool worker
    // (Async::operator() asserts exactly that).
    auto [inner_res, gas] =
        wrap_after_parse<SyscallMeta>(guard.check(), handler->to_handler(),
                                      gas_runtime->gas_remaining(), syscall_no,
                                      std::move(parsed_args))();
    gas_runtime->consume_gas(gas);

    return from_status(inner_res);
  }(from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{}));
}

} // namespace hogd
