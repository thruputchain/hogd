#include "hogd/test_ffi/test_bindings.h"

#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/auth_claim/status.h"
#include "hogd/auth_claim/verify.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/shared.h"
#include "hogd/init.h"
#include "hogd/metrics/metrics.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/ffi_data_buffer.h"
#include "hogd/test_ffi/ffi_error.h"
#include "hogd/test_ffi/formatter.h" // IWYU pragma: keep
#include "hogd/test_ffi/logger.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/mock_syscall_entry.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "storage_cost.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/debug.h"
#include "x23/format.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// The functions here generally log the caught errors and return
// appropriate TestFFIErrorCodes.
// All of the FFI functions are additionally marked noexcept, in case
// there are additional unhandled exceptions.
// Unhandled exception types, if thrown, are considered a bug in these
// FFI bindings, and the bindings should be updated.

template<typename FFIResultType>
concept ffi_result_type = requires(FFIResultType result) {
  { result.ffi_error_code } -> std::convertible_to<TestFFIErrorCode>;
};

template<ffi_result_type FFIResultType>
constexpr FFIResultType
make_ffi_result_type(TestFFIErrorCode ffi_error_code)
{
  return {.ffi_error_code = ffi_error_code};
}

ThreadsafeLogger *global_logger = nullptr;
x23::DebugFd<"TEST_FFI_TRACE"> ffi_trace;

template<typename... Args>
void
do_ffi_trace(std::format_string<Args...> fmt, Args &&...args) noexcept
{
  try {
    ffi_trace(fmt, std::forward<Args>(args)...);
  } catch (std::exception const &e) {
    // FFI tracing is best-effort diagnostics, and the start-of-call trace in
    // each exported wrapper runs before the first-use helper reaches
    // lazy_init(). Routing this through error() would dereference the still-
    // null process logger and terminate an honest caller (issue #2478), so
    // report the failure straight to stderr without allocating.
    std::fputs("error tracing ffi: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputc('\n', stderr);
  }
}

// Run a best-effort diagnostic log step; on failure report to stderr without
// allocating rather than letting the exception escape.
//
// Several exported FFI helpers are noexcept and reject misuse (a still-open
// handler, an already-finalized transaction, a null handle) with a structured
// TestFFIErrorCode. Those rejection paths log via error() *before* the
// function's try/catch boundary, and error() routes into the process logger,
// which allocates a token vector and an output string while formatting the
// message. A std::bad_alloc there would otherwise cross the noexcept boundary
// and terminate an honest caller instead of returning the structured error
// (issue #2559). Wrapping the guard diagnostic keeps it best-effort: the
// message is emitted when memory allows, and a logging failure is reported via
// fputs (no allocation -- same fallback as do_ffi_trace) so it is never
// silently dropped while still letting the guard return its FFI error code.
template<typename Fn>
void
best_effort_log(Fn &&fn) noexcept
{
  try {
    std::forward<Fn>(fn)();
  } catch (std::exception const &e) {
    std::fputs("error emitting test FFI guard diagnostic: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputc('\n', stderr);
  }
}

// First-use initialization for the test_ffi entry points: runs hogd_init(),
// allocates the ThreadsafeLogger, and swaps it in as the process logger. Every
// exported entry point calls this on first use.
//
// Those steps allocate, so they can throw std::bad_alloc. Several entry points
// reach lazy_init() from a noexcept helper, before their own try/catch
// boundary; a throw there would cross the noexcept boundary and terminate an
// honest caller instead of returning a structured error (issues #2441, #2464,
// #2514, #2575). So lazy_init() is itself noexcept and reports failure via its
// bool return -- callers translate false into their own
// kTestFFIErrorCodeUnrecoverableError. Failure is reported best-effort to
// stderr because the process logger does not exist yet: error() would
// dereference a still-null logger.
[[nodiscard]] bool
lazy_init() noexcept
{
  try {
    static std::mutex init_mutex;
    std::lock_guard lock(init_mutex);
    static bool initialized = false;
    // init_hogexec_metrics() registers several metrics and is not atomic: if it
    // throws partway (e.g. bad_alloc mid-registration) it leaves the executor
    // component partially populated with done_with_init still false. A naive
    // retry would re-register a duplicate name and trip init_metric()'s CHECK
    // (an abort, defeating this noexcept initializer's structured-error
    // contract). Latch that as a permanent failure and keep reporting it
    // instead of retrying into the abort.
    static bool metrics_init_failed = false;
    if (metrics_init_failed) {
      std::fputs("test_ffi: executor metrics initialization previously failed; "
                 "the library is unusable\n",
                 stderr);
      return false;
    }
    ffi_trace("lazy_init: initialized={}", initialized);
    if (!initialized) {
      hogd_init(0, nullptr, false, LogLevel::kCritical);
      auto logger = std::make_unique<ThreadsafeLogger>(LogLevel::kCritical);
      global_logger = logger.get();
      hogd::swap_logger(std::move(logger));
      // The runtime records executor metrics during transaction commit
      // (HogexecMetrics::mark_tx_*), so a committing entry point trips Metrics'
      // done_with_init CHECK unless those metrics are registered first. Guarded
      // because a host that already initialized them (e.g. a test main calling
      // init_hogexec_metrics()) must not double-initialize: init_metric()
      // asserts the component is not yet marked done.
      if (!Metrics::instance().done_with_init(
              Metrics::ComponentName::kExecutor)) {
        metrics_init_failed = true; // pessimistic; cleared only on full success
        init_hogexec_metrics();
        metrics_init_failed = false;
      }
      initialized = true;
    }
    return true;
  } catch (std::exception const &e) {
    std::fputs("error initializing test_ffi logging: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputc('\n', stderr);
    return false;
  }
}

void
enable_syscall_logging(int32_t level) noexcept
{
  LogLevel log_level;
  switch (static_cast<LogLevel>(level)) {
  case LogLevel::kCritical:
    static_assert(
        static_cast<int32_t>(TestFFILogLevel::kTestFFILogLevelCritical) ==
        static_cast<int32_t>(LogLevel::kCritical));
    log_level = LogLevel::kCritical;
    break;
  case LogLevel::kError:
    static_assert(
        static_cast<int32_t>(TestFFILogLevel::kTestFFILogLevelError) ==
        static_cast<int32_t>(LogLevel::kError));
    log_level = LogLevel::kError;
    break;
  case LogLevel::kWarn:
    static_assert(static_cast<int32_t>(TestFFILogLevel::kTestFFILogLevelWarn) ==
                  static_cast<int32_t>(LogLevel::kWarn));
    log_level = LogLevel::kWarn;
    break;
  case LogLevel::kInfo:
    static_assert(static_cast<int32_t>(TestFFILogLevel::kTestFFILogLevelInfo) ==
                  static_cast<int32_t>(LogLevel::kInfo));
    log_level = LogLevel::kInfo;
    break;
  case LogLevel::kDebug:
    static_assert(
        static_cast<int32_t>(TestFFILogLevel::kTestFFILogLevelDebug) ==
        static_cast<int32_t>(LogLevel::kDebug));
    log_level = LogLevel::kDebug;
    break;
  case LogLevel::kTrace:
    static_assert(
        static_cast<int32_t>(TestFFILogLevel::kTestFFILogLevelTrace) ==
        static_cast<int32_t>(LogLevel::kTrace));
    log_level = LogLevel::kTrace;
    break;
  default:
    log_level = LogLevel::kInfo;
    break;
  }

  // First-use init can fail (allocation failure); lazy_init() reports it to
  // stderr and returns false. This helper has no status return, so just leave
  // logging unchanged (issue #2441).
  if (!lazy_init()) {
    return;
  }
  assert(global_logger != nullptr);
  global_logger->set_log_level(log_level);
}

CreateDBResult
make_new_db(const uint8_t *serialized_cost_model,
            uint32_t serialized_cost_model_len) noexcept
{
  // First-use init can fail (allocation failure); return a structured error
  // rather than letting it cross this noexcept boundary (issue #2575).
  if (!lazy_init()) {
    return {.db = nullptr,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
  try {
    StorageCostModelXdr cost_model_xdr =
        StorageCostModel::default_cost_model().to_xdr();
    if (serialized_cost_model != nullptr) {
      try {
        x23::deserialize(&cost_model_xdr,
                         x23::BufView{serialized_cost_model,
                                      serialized_cost_model_len});
      } catch (x23::XdrException const &e) {
        log_error("error deserializing cost model: {}", e);
        return {.db = nullptr,
                .ffi_error_code = kTestFFIErrorCodeDeserializeError};
      }
    }
    auto *db = new DBContext(cost_model_xdr);
    return {.db = db, .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (std::exception const &e) {
    log_error("error creating db context: {}", e);
    return {.db = nullptr,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
}

GetCostModelResult
get_cost_model(void *db) noexcept
{
  auto *db_context = reinterpret_cast<hogd::DBContext *>(db);
  if (!db_context) {
    return {.data = kNullBuffer,
            .ffi_error_code = kTestFFIErrorCodeInvalidPointer};
  }

  try {
    // Reading the cost model copies the persisted RAMDB row, which allocates
    // and can throw std::bad_alloc. Keep it inside the catch boundary so it is
    // converted to a structured FFI error instead of crossing the noexcept
    // helper and terminating the process (issue #2513).
    auto cost_model = db_context->get_base_db().get_cost_model().to_xdr();
    auto serialized = x23::serialize(cost_model);

    auto buffer = make_new_data_buffer(serialized.size());
    if (!buffer.has_value()) {
      return {.data = kNullBuffer, .ffi_error_code = buffer.error()};
    }
    std::memcpy(buffer.value().data, serialized.data(), serialized.size());
    return {.data = buffer.value(), .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (std::exception const &e) {
    log_error("error serializing cost model: {}", e);
    return {.data = kNullBuffer,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
}

GetSnapshotStorageItemResult
get_snapshot_storage_item(void *db, FFISerializedAddressAndKey addrkey) noexcept
{
  auto *db_context = reinterpret_cast<hogd::DBContext *>(db);
  if (!db_context || addrkey.data == nullptr) {
    return {.data = kNullBuffer,
            .ffi_error_code = kTestFFIErrorCodeInvalidPointer};
  }

  try {
    AddressAndKey deserialized_addrkey;
    try {
      x23::deserialize(&deserialized_addrkey,
                       x23::BufView{addrkey.data, addrkey.len});
    } catch (x23::XdrException const &e) {
      log_error("error deserializing address and key: {}", e);
      return {.data = kNullBuffer,
              .ffi_error_code = kTestFFIErrorCodeDeserializeError};
    }

    auto item = db_context->get_snapshot_item(deserialized_addrkey);
    if (!item.has_value()) {
      return {.data = kNullBuffer, .ffi_error_code = kTestFFIErrorCodeNone};
    }

    auto serialized = serialize_to_ffi_data_buffer(item.value());
    if (!serialized.has_value()) {
      return {.data = kNullBuffer, .ffi_error_code = serialized.error()};
    }

    return {.data = serialized.value(),
            .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (std::exception const &e) {
    log_error("error getting snapshot storage item: {}", e);
    return {.data = kNullBuffer,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
}

TestFFIErrorCode
free_db(DBContext *ptr) noexcept
{
  // Syscall handlers borrow the DB (and its snapshot/proxies) by reference, so
  // freeing it while a handler is still open would dangle those references on a
  // later close/invoke. Reject the free instead of crashing (issue #2433).
  if (ptr != nullptr && ptr->has_open_handlers()) {
    best_effort_log([] {
      error("free_db called while syscall handlers derived from this DB are "
            "still open; close them first");
    });
    return kTestFFIErrorCodeUnrecoverableError;
  }
  try {
    delete ptr;
  } catch (std::exception const &e) {
    log_error("error freeing db context: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
  return kTestFFIErrorCodeNone;
}

TestFFIErrorCode
advance_new_block(void *db) noexcept
{
  auto *ptr = reinterpret_cast<DBContext *>(db);
  if (!ptr)
    return kTestFFIErrorCodeInvalidPointer;

  // Advancing the block replaces the snapshot cache that open handlers still
  // reference; reject it while any handler derived from this DB is open so a
  // later root_invoke() doesn't read freed snapshot state (issue #2436).
  if (ptr->has_open_handlers()) {
    best_effort_log([] {
      error("advance_new_block called while syscall handlers derived from this "
            "DB are still open; close them first");
    });
    return kTestFFIErrorCodeUnrecoverableError;
  }

  // A prior close_syscall_handler() failed to confirm closure of its HogDB
  // transaction and poisoned this DB (issue #2596). The open-handler guard is
  // gone, but an orphaned InvokeTX may still hold a BlockTX open, so advancing
  // (which closes/commits the current snapshot and opens a new one) is unsafe.
  // Refuse it; free_db() remains the only safe operation on a poisoned DB.
  if (ptr->is_lifecycle_poisoned()) {
    best_effort_log([] {
      error("advance_new_block called on a DB poisoned by a failed handler "
            "close; the DB can no longer be advanced and must be freed");
    });
    return kTestFFIErrorCodeUnrecoverableError;
  }

  try {
    ptr->advance_new_block();
    return kTestFFIErrorCodeNone;
  } catch (std::exception const &e) {
    log_error("error advancing new block: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
}

OpenRuntimeContextResult
open_runtime_context() noexcept
{
  // First-use init can fail (allocation failure); return a structured error
  // rather than letting it cross this noexcept boundary (issue #2575).
  if (!lazy_init()) {
    return {.context = nullptr,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
  try {
    // Own the wrapper slot via unique_ptr until construction fully succeeds, so
    // a throw from make_shared() (or make_hogexec_context()) after the slot is
    // allocated does not leak it. The honest C caller gets a null handle on
    // failure and has no way to free the slot itself (issue #2506).
    auto out = std::make_unique<std::shared_ptr<hogd::MockRuntimeContext>>();
    *out = std::make_shared<hogd::MockRuntimeContext>(make_hogexec_context());
    CHECK(*out != nullptr);
    return {.context = out.release(), .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (std::exception const &e) {
    log_error("error creating runtime context: {}", e);
    return {.context = nullptr,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
}

TestFFIErrorCode
close_runtime_context(void *context) noexcept
{
  try {
    delete static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);
    return kTestFFIErrorCodeNone;
  } catch (std::exception const &e) {
    log_error("error closing runtime context: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
}

TestFFIErrorCode
register_new_contract(void *context, int32_t mock_contract_id,
                      ForeignEntryPoint entry_point,
                      void *foreign_context) noexcept
{
  if (context == nullptr || entry_point == nullptr)
    return kTestFFIErrorCodeInvalidPointer;

  auto *c = static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);
  if (!c)
    return kTestFFIErrorCodeInvalidPointer;
  CHECK(*c != nullptr);

  try {
    // set_trampoline() inserts into a heap-allocating std::map, which can throw
    // std::bad_alloc. Without this boundary the exception would cross the
    // noexcept FFI helper and terminate an honest caller (issue #2489).
    (*c)->set_trampoline(mock_contract_id, entry_point, foreign_context);
  } catch (std::exception const &e) {
    log_error("error registering mock contract: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
  return kTestFFIErrorCodeNone;
}

OpenSyscallHandlerResult
open_syscall_handler(void *db, void *rc,
                     FFISerializedSignedTransaction signed_tx) noexcept
{
  constexpr auto kNullptrError = make_ffi_result_type<OpenSyscallHandlerResult>(
      kTestFFIErrorCodeInvalidPointer);

  if (!db) {
    return kNullptrError;
  }
  auto *db_context = reinterpret_cast<hogd::DBContext *>(db);
  if (!rc) {
    return kNullptrError;
  }
  auto *runtime_context =
      reinterpret_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(rc);

  CHECK(runtime_context->get() != nullptr);

  if (!signed_tx.data) {
    return kNullptrError;
  }
  try {
    hogd::SignedTransaction stx;
    x23::deserialize(&stx, x23::BufView{signed_tx.data, signed_tx.len});
    auto *out = static_cast<void *>(
        new SyscallHandlerContext(*runtime_context, stx, *db_context));
    return {.syscall_handler = out, .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (x23::XdrException const &e) {
    log_error("error deserializing signed transaction: {}", e);
    return make_ffi_result_type<OpenSyscallHandlerResult>(
        kTestFFIErrorCodeDeserializeError);
  } catch (std::exception const &e) {
    log_error("error creating syscall handler context: {}", e);
    return make_ffi_result_type<OpenSyscallHandlerResult>(
        kTestFFIErrorCodeUnrecoverableError);
  }
}

OpenSyscallHandlerResult
open_mocked_syscall_handler(void *db, void *rc,
                            struct FFISerializedAuthClaimData auth_claim_data,
                            const uint8_t source_tx_hash[32],
                            uint8_t enable_tracing) noexcept
{
  constexpr auto kNullptrError = make_ffi_result_type<OpenSyscallHandlerResult>(
      kTestFFIErrorCodeInvalidPointer);

  if (!db) {
    return kNullptrError;
  }
  auto *db_context = reinterpret_cast<hogd::DBContext *>(db);
  if (!rc) {
    return kNullptrError;
  }
  auto *runtime_context =
      reinterpret_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(rc);

  CHECK(runtime_context->get() != nullptr);

  if (!auth_claim_data.data || !source_tx_hash) {
    return kNullptrError;
  }
  try {
    hogd::AuthClaim auth_claim_xdr;
    x23::deserialize(&auth_claim_xdr,
                     x23::BufView{auth_claim_data.data, auth_claim_data.len});
    SHA256Hash source_hash{};
    std::memcpy(source_hash.data(), source_tx_hash, source_hash.size());
    auto handler =
        std::make_unique<SyscallHandlerContext>(*runtime_context, *db_context,
                                                auth_claim_xdr, source_hash);
    if (enable_tracing) {
      // enable_tracing() allocates an InvocationFrameTrace and can throw. The
      // SyscallHandlerContext constructor has already opened the transaction
      // and registered an open-handler borrow on the DB, and
      // ~SyscallHandlerContext CHECKs that the transaction was committed or
      // aborted, so a leaked raw handler here would both pin the DB (blocking
      // advance/free) and be unclosable by a caller who never received a handle
      // (issue #2564). Tear the handler down through the same
      // abort-then-destroy path as close_syscall_handler() before unwinding to
      // a structured error.
      try {
        handler->to_handler().enable_tracing();
      } catch (...) {
        if (!handler->is_committed_or_aborted()) {
          handler->mark_committed_or_aborted();
          // Best-effort abort; we are already unwinding to an FFI error, and
          // the destructor only requires the transaction be marked finalized.
          // Report a failed abort -- whether it returns an error status or
          // throws -- rather than silently dropping it.
          try {
            auto abort_res = handler->to_handler().abort_transaction();
            if (!abort_res.has_value()) {
              best_effort_log([&] {
                error("error aborting transaction during traced "
                      "open_mocked_syscall_handler cleanup: {}",
                      abort_res.error());
              });
            }
          } catch (std::exception const &abort_err) {
            std::fputs("error aborting transaction during traced "
                       "open_mocked_syscall_handler cleanup: ",
                       stderr);
            std::fputs(abort_err.what(), stderr);
            std::fputc('\n', stderr);
          }
        }
        throw;
      }
    }
    return {.syscall_handler = static_cast<void *>(handler.release()),
            .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (x23::XdrException const &e) {
    log_error("error deserializing signed transaction: {}", e);
    return make_ffi_result_type<OpenSyscallHandlerResult>(
        kTestFFIErrorCodeDeserializeError);
  } catch (std::exception const &e) {
    log_error("error creating syscall handler context: {}", e);
    return make_ffi_result_type<OpenSyscallHandlerResult>(
        kTestFFIErrorCodeUnrecoverableError);
  }
}

TestFFIErrorCode
close_syscall_handler(void *syscall_handler) noexcept
{
  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
  if (context == nullptr) {
    return kTestFFIErrorCodeNone;
  }
  if (!context->can_be_closed()) {
    return kTestFFIErrorCodeUnrecoverableError;
  }

  // Reference the borrowed DB before taking ownership: if the implicit abort
  // below fails (or throws), the handler -- and with it its open-handler guard
  // -- is destroyed, but the HogDB InvokeTX may still be open. Poison the DB's
  // lifecycle through this reference so advance_new_block() cannot proceed past
  // the now-absent guard into the scheduler over orphaned transaction state
  // (issue #2596). The DBContext is owned by the caller and outlives the
  // handler, so this reference stays valid after `owned` is destroyed,
  // including during exception unwind.
  DBContext &db = context->borrowed_db();

  // Take ownership so the handler is freed on every path below, including when
  // the abort reports an error or throws.
  std::unique_ptr<SyscallHandlerContext> owned(context);

  try {
    // The handler destructor requires the transaction to have been committed or
    // aborted. Abort any still-open transaction here, inside this try/catch, so
    // a throwing abort RPC becomes kTestFFIErrorCodeUnrecoverableError rather
    // than terminating the process from the destructor's noexcept boundary
    // (issue #2488). Mark aborted before issuing the RPC: the handler also
    // marks itself aborted up front, so a retry after a thrown abort must not
    // attempt a second abort.
    if (!owned->is_committed_or_aborted()) {
      owned->mark_committed_or_aborted();
      auto res = owned->to_handler().abort_transaction();
      if (!res.has_value()) {
        error("error aborting transaction during close: {}", res.error());
        db.poison_lifecycle();
        return kTestFFIErrorCodeUnrecoverableError;
      }
    }

    return kTestFFIErrorCodeNone;
  } catch (std::exception const &e) {
    log_error("error closing syscall handler: {}", e);
    db.poison_lifecycle();
    return kTestFFIErrorCodeUnrecoverableError;
  }
}

constexpr ForeignTracedInvokeResult kUnrecoverableError = {
    .serialized_output = kNullBuffer,
    .gas_consumed = 0,
    .serialized_trace = kNullBuffer,
    .serialized_events = kNullBuffer,
    .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};

ForeignTracedInvokeResult
root_invoke(void *syscall_handler, bool enable_tracing) noexcept
{
  if (!syscall_handler) {
    best_effort_log([] { error("root_invoke error: no syscall handler!"); });
    return make_ffi_result_type<ForeignTracedInvokeResult>(
        kTestFFIErrorCodeInvalidPointer);
  }

  try {
    auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
    // Hand enable_tracing to root_invoke() rather than applying it here:
    // SystemCallHandler::enable_tracing() asserts the invocation-frame stack is
    // empty, but a mocked handler may already have a mock frame open. Enabling
    // tracing before root_invoke()'s mocked-handler guard runs would trip that
    // assert instead of returning the structured rejection (issue #2601).
    auto res = context->root_invoke(enable_tracing);
    return res;
  } catch (std::exception const &e) {
    log_error("unrecoverable: unhandled error during root_invoke: {}", e);
    return kUnrecoverableError;
  } catch (...) {
    // A mock contract callback (ForeignEntryPoint) is not declared noexcept and
    // is not restricted to std::exception throws;
    // MockRuntime::invoke_internal() lets callback exceptions unwind. A
    // non-std::exception throw (e.g. int) would otherwise escape this noexcept
    // boundary and terminate an honest caller (#2693), so translate it to a
    // structured error -- as mock_syscall_entrypoint() already does. Report via
    // fputs (no allocation), since there is no exception object to format.
    std::fputs(
        "test_ffi: unrecoverable: unknown exception during root_invoke\n",
        stderr);
    return kUnrecoverableError;
  }
}

TestFFIErrorCode
open_mock_invocation_frame(void *syscall_handler,
                           struct FFISerializedInvocation invocation,
                           int32_t invoke_flags) noexcept
{
  if (!syscall_handler) {
    best_effort_log(
        [] { error("open_mock_invocation_frame error: no syscall handler!"); });
    return kTestFFIErrorCodeInvalidPointer;
  }
  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);

  Invocation invocation_xdr;
  try {
    x23::deserialize(&invocation_xdr,
                     x23::BufView{invocation.data, invocation.len});
  } catch (x23::XdrException const &e) {
    log_error("error deserializing invocation: {}", e);
    return kTestFFIErrorCodeDeserializeError;
  } catch (std::exception const &e) {
    log_error("unrecoverable: error during open_mock_invocation_frame: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }

  try {
    auto res =
        context->start_mock_invocation_frame(invocation_xdr, invoke_flags);
    if (res != kTestFFIErrorCodeNone) {
      error("failed to start mock invocation frame: {}",
            std::to_underlying(res));
    }
    return res;
  } catch (std::exception const &e) {
    log_error("unrecoverable: error during start_mock_invocation_frame: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
}

ForeignTracedInvokeResult
finish_mock_invocation_frame(void *syscall_handler,
                             int32_t invoke_error_code) noexcept
{
  if (!syscall_handler) {
    best_effort_log([] {
      error("finish_mock_invocation_frame error: no syscall handler!");
    });
    return make_ffi_result_type<ForeignTracedInvokeResult>(
        kTestFFIErrorCodeInvalidPointer);
  }
  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);

  try {
    return context->finish_mock_invocation_frame(
        static_cast<InvokeErrorCode>(invoke_error_code));
  } catch (std::exception const &e) {
    log_error("unrecoverable: error during finish_mock_invocation_frame: {}",
              e);
    return make_ffi_result_type<ForeignTracedInvokeResult>(
        kTestFFIErrorCodeUnrecoverableError);
  }
}

TestFFIErrorCode
push_context(void *syscall_handler)
{
  if (!syscall_handler) {
    best_effort_log([] { error("push_context error: no syscall handler!"); });
    return kTestFFIErrorCodeInvalidPointer;
  }
  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
  if (!context->push_context()) {
    best_effort_log([] { error("failed to push context"); });
    return kTestFFIErrorCodeUnrecoverableError;
  }
  return kTestFFIErrorCodeNone;
}

TestFFIErrorCode
pop_context()
{
  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    best_effort_log([] { error("pop_context error: no context!"); });
    return kTestFFIErrorCodeUnrecoverableError;
  }
  // The thread-local context installed for a running root_invoke() is owned by
  // that call; a mock contract callback must not pop it, or root_invoke()'s
  // own deferred pop would CHECK-fail on the now-empty slot (issue #2510).
  if (context->root_invoke_owns_context()) {
    best_effort_log([] {
      error(
          "pop_context error: context is owned by the running root invocation");
    });
    return kTestFFIErrorCodeUnrecoverableError;
  }
  context->pop_context();
  return kTestFFIErrorCodeNone;
}

ForeignTryCommitResult
try_commit_transaction(void *syscall_handler) noexcept
{
  constexpr auto kUnrecoverableError =
      make_ffi_result_type<ForeignTryCommitResult>(
          kTestFFIErrorCodeUnrecoverableError);

  if (!syscall_handler) {
    best_effort_log(
        [] { error("try_commit_transaction error: no syscall handler!"); });
    return make_ffi_result_type<ForeignTryCommitResult>(
        kTestFFIErrorCodeInvalidPointer);
  }

  try {
    auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
    // Reject a repeat commit (or a commit after a prior abort) on a still-live
    // handle. Like the abort path, SystemCallHandler::try_commit_transaction()
    // treats finalization as single-use and asserts
    // !transaction_committed_or_aborted_, so re-entering it after the
    // transaction is already terminal would crash assertion-enabled builds
    // (issue #2518). The last_frame_finished() guard below does not cover this:
    // it stays satisfied after a successful commit. Consult the wrapper's
    // terminal flag first.
    if (context->is_committed_or_aborted()) {
      error("try_commit_transaction called on an already committed/aborted "
            "transaction");
      return kUnrecoverableError;
    }
    // try_commit_transaction() requires the root invocation (or, for a mocked
    // handler, the root mock frame) to have finished first; otherwise the
    // internal commit path asserts last_frame_.has_value() and aborts. Reject
    // commit-before-root with a structured FFI error instead (issue #2437).
    if (!context->to_handler().last_frame_finished()) {
      error("try_commit_transaction called before root invocation finished");
      return kUnrecoverableError;
    }
    // A prior finish_mock_invocation_frame() may have finalized the root frame
    // but then failed to hand its result back to the caller (e.g. allocation
    // failure during FFI serialization). Committing now would commit a result
    // the caller never received, so reject it (issue #2508).
    if (context->finish_result_handoff_failed()) {
      error("try_commit_transaction after a failed finish-result handoff");
      return kUnrecoverableError;
    }
    auto res = context->to_handler().try_commit_transaction();
    context->mark_committed_or_aborted();
    if (res == ExecResult::ExecResultOK) {
      auto events_res =
          context->to_handler().emit_events(ExecResult::ExecResultOK,
                                            ProposedTransaction{})();
      if (!events_res.has_value()) {
        error("failed to emit events: {}", events_res.error());
        return ForeignTryCommitResult{
            .result_code = std::to_underlying(
                SystemCallHandler::to_exec_result(events_res.error().code()))};
      }
    }
    return ForeignTryCommitResult{.result_code = std::to_underlying(res)};
  } catch (std::exception const &e) {
    log_error("error during try_commit_transaction: {}", e);
    return kUnrecoverableError;
  }
}

TestFFIErrorCode
abort_transaction(void *syscall_handler) noexcept
{
  if (!syscall_handler) {
    best_effort_log(
        [] { error("abort_transaction error: no syscall handler!"); });
    return kTestFFIErrorCodeInvalidPointer;
  }
  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);

  // Reject a repeat abort (or an abort after commit) on a still-live handle.
  // SystemCallHandler::abort_transaction() treats finalization as single-use
  // and asserts !transaction_committed_or_aborted_, so re-entering the core
  // abort after the first one terminates assertion-enabled builds instead of
  // returning a structured error (issue #2518). The wrapper already tracks the
  // terminal state for destructor cleanup; consult it here before forwarding.
  if (context->is_committed_or_aborted()) {
    best_effort_log([] {
      error("abort_transaction called on an already committed/aborted "
            "transaction");
    });
    return kTestFFIErrorCodeUnrecoverableError;
  }

  // Reject abort while an invocation frame is still executing. The handler is
  // either mid root_invoke -- possibly reentrantly, from a mock contract
  // callback that holds the handler pointer (issue #2511) -- or has an open
  // mock invocation frame (issue #2509). Finalizing the transaction underneath
  // a running frame corrupts the handler (a later successful root frame then
  // trips try_commit_transaction's assert) and leaves mock-frame state that
  // blocks close. Surface a structured error and leave the handler untouched so
  // the caller can finish its frames first.
  if (context->to_handler().is_running()) {
    best_effort_log([] {
      error(
          "abort_transaction called while an invocation frame is still active");
    });
    return kTestFFIErrorCodeUnrecoverableError;
  }

  try {
    auto res = context->to_handler().abort_transaction();
    // SystemCallHandler::abort_transaction() marks its transaction aborted
    // before issuing the abort RPC, so the context must record the abort even
    // when the RPC reports or throws an error; otherwise the destructor in
    // close_syscall_handler() would attempt a second abort.
    context->mark_committed_or_aborted();
    if (!res.has_value()) {
      error("failed to abort transaction: {}", res.error());
      return kTestFFIErrorCodeUnrecoverableError;
    }
    return kTestFFIErrorCodeNone;
  } catch (std::exception const &e) {
    // The abort RPC can throw (e.g. an SRPC XdrBadInput/XdrCallFailed on a bad
    // reply); convert it to a structured error instead of letting it cross the
    // noexcept FFI boundary and terminate the process (issue #2443).
    context->mark_committed_or_aborted();
    log_error("error during abort_transaction: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
}

constexpr StorageCostModel kCostModel(12000, 1, 5, 5, 1234);

TestFFIErrorCode
setup_grant_contract_storage_tokens(void *db, const uint8_t *contract_addr,
                                    int64_t tokens) noexcept
{
  if (db == nullptr || contract_addr == nullptr) {
    return kTestFFIErrorCodeInvalidPointer;
  }
  if (tokens < 0) {
    best_effort_log([tokens] {
      error("setup_grant_contract_storage_tokens: tokens must be nonnegative, "
            "got {}",
            tokens);
    });
    return kTestFFIErrorCodeInvalidArgument;
  }
  auto *db_cast = static_cast<DBContext *>(db);
  hogd::ContractAddress addr;
  std::memcpy(addr.data(), contract_addr, addr.size());

  try {
    trace("granting {} storage tokens for address {}", tokens, x23::show(addr));
    db_cast->get_base_db().grant_storage_token(addr, tokens);
  } catch (std::exception const &e) {
    log_error("error granting storage tokens: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }

  return kTestFFIErrorCodeNone;
}

TestFFIErrorCode
setup_create_contract(void *context, void *db, uint8_t const *contract_bytes,
                      uint32_t contract_len) noexcept
{
  if (context == nullptr || db == nullptr) {
    return kTestFFIErrorCodeInvalidPointer;
  }
  auto *context_cast =
      static_cast<std::shared_ptr<MockRuntimeContext> *>(context);
  auto *db_cast = static_cast<DBContext *>(db);

  Contract contract;
  try {
    x23::deserialize(&contract, x23::BufView{contract_bytes, contract_len});
  } catch (x23::XdrException &e) {
    log_error("error deserializing contract: {}", e);
    return kTestFFIErrorCodeDeserializeError;
  } catch (std::exception const &e) {
    // Deserializing a well-formed contract still allocates (e.g. resizing the
    // ContractBlob::wasm vector), so std::bad_alloc can be thrown here on valid
    // input. Catch it so it doesn't cross the noexcept FFI boundary (#2490).
    log_error("unrecoverable: error deserializing contract: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }

  try {
    // make sure the contract has already been registered with the
    // instantiation context
    if (contract.contract_type() == ContractType::kMockTest) {
      if (!((*context_cast)->has_mock_contract(contract.mock_contract_id()))) {
        error(
            "contract has not been registered with the instantiation context");
        return kTestFFIErrorCodeCreateNonexistentMockContract;
      }
    }

    // Verify runtime-backed contracts before storing — reject contracts that
    // would fail during instantiation (e.g., wasm memory exceeding limits, an
    // RV32 artifact that cannot be translated, or an unknown native id).
    // kMockTest is excluded because it has no production runtime to verify
    // against (the base context only registers the real runtimes); its
    // registration is checked above via has_mock_contract().
    if (contract.contract_type() != ContractType::kMockTest) {
      auto verify_result = (*context_cast)->verify_contract(contract);
      if (verify_result == InvokeErrorCode::Unrecoverable) {
        return kTestFFIErrorCodeUnrecoverableError;
      }
      if (verify_result != InvokeErrorCode::None) {
        return kTestFFIErrorCodeContractVerificationFailed;
      }
    }

    auto h = sha256(contract);

    db_cast->get_base_db().write_contract(h, std::make_shared<Contract>(
                                                 std::move(contract)));
  } catch (std::exception const &e) {
    log_error("error creating contract: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
  return kTestFFIErrorCodeNone;
}

TestFFIErrorCode
setup_deploy_contract(void *db, const uint8_t *deployment_addr,
                      const uint8_t *contract_hash,
                      uint32_t contract_size) noexcept
{
  if (db == nullptr || contract_hash == nullptr || deployment_addr == nullptr) {
    return kTestFFIErrorCodeInvalidPointer;
  }
  auto *db_cast = static_cast<DBContext *>(db);

  SHA256Hash h;
  std::memcpy(h.data(), contract_hash, h.size());
  ContractAddress addr;
  std::memcpy(addr.data(), deployment_addr, addr.size());

  auto &dao = db_cast->get_base_db().get_db();
  Contract contract;
  try {
    // read_wasm_code() copies the stored row out of RAMDB (make_byte_buffer()
    // allocates the returned byte buffer, #2765) and x23::deserialize() resizes
    // the opaque-vector destination (#2642/#2766); both can throw
    // std::bad_alloc on a valid stored contract. setup_deploy_contract() is
    // noexcept, so wrap the read and decode in one exception-translation
    // boundary -- mirroring setup_create_contract() -- rather than letting an
    // allocation failure cross the noexcept boundary and terminate an honest
    // caller.
    auto res = dao.read_wasm_code(h);
    if (!res) {
      // best_effort_log keeps the not-found diagnostic non-throwing so it still
      // returns the structured missing-contract code (#2584, same pattern as
      // #2559) even under memory pressure.
      best_effort_log([] {
        error("Attempt to deploy contract that has not been created in the "
              "database");
      });
      return kTestFFIErrorCodeDeployNonexistentContract;
    }
    x23::deserialize(&contract,
                     x23::BufView{res.value().data(), res.value().size()});
  } catch (x23::XdrException &e) {
    log_error("unrecoverable: error deserializing contract from db: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  } catch (std::exception const &e) {
    log_error("unrecoverable: error reading/deserializing stored contract: {}",
              e);
    return kTestFFIErrorCodeUnrecoverableError;
  }

  try {
    // write_deployment() throws (e.g. std::runtime_error) when the underlying
    // metadata write fails, such as RAMDB kNoMemory. Convert that to a
    // structured error instead of letting it cross the noexcept FFI boundary
    // and terminate the process (issue #2480).
    db_cast->get_base_db().write_deployment(
        addr,
        DeployedContractHash{.hash = h,
                             .contract_size = contract_size,
                             .contract_type = contract.contract_type()},
        &kCostModel);
  } catch (std::exception const &e) {
    log_error("error writing contract deployment: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }

  return kTestFFIErrorCodeNone;
}

// Helper function to deserialize a SignedTransaction
template<typename T>
std::optional<SignedTransaction>
deserialize_signed_transaction(FFISerializedSignedTransaction signed_tx,
                               T *error_out)
{
  if (!signed_tx.data) {
    if (error_out) {
      *error_out = make_ffi_result_type<T>(kTestFFIErrorCodeInvalidPointer);
    }
    return std::nullopt;
  }

  try {
    SignedTransaction stx;
    x23::deserialize(&stx, x23::BufView{signed_tx.data, signed_tx.len});
    return stx;
  } catch (x23::XdrException const &e) {
    log_error("error deserializing signed transaction: {}", e);
    if (error_out) {
      *error_out = make_ffi_result_type<T>(kTestFFIErrorCodeDeserializeError);
    }
    return std::nullopt;
  } catch (std::exception const &e) {
    log_error("unexpected error deserializing signed transaction: {}", e);
    if (error_out) {
      *error_out = make_ffi_result_type<T>(kTestFFIErrorCodeUnrecoverableError);
    }
    return std::nullopt;
  }
}

VerifyAuthResult
verify_signed_transaction_auth_impl(FFISerializedSignedTransaction signed_tx,
                                    FFISourceAuthData source_auth_data) noexcept
{
  constexpr auto kNullptrError =
      make_ffi_result_type<VerifyAuthResult>(kTestFFIErrorCodeInvalidPointer);

  VerifyAuthResult error_result;
  auto stx_opt = deserialize_signed_transaction(signed_tx, &error_result);
  if (!stx_opt) {
    return error_result;
  }
  const SignedTransaction &stx = *stx_opt;

  try {
    // Deserialize source auth data
    if (!source_auth_data.data) {
      return kNullptrError;
    }

    AddressAuthData source_data;
    x23::deserialize(&source_data,
                     x23::BufView{source_auth_data.data, source_auth_data.len});

    // Perform the appropriate level of verification
    AuthVerifyStatus<void> result = verify_auth(stx, source_data);

    if (result.has_value()) {
      return {.auth_error_code = 0, .ffi_error_code = kTestFFIErrorCodeNone};
    }
    return {.auth_error_code = std::to_underlying(result.error().code()),
            .ffi_error_code = kTestFFIErrorCodeNone};

  } catch (x23::XdrException const &e) {
    log_error("error deserializing auth data: {}", e);
    return make_ffi_result_type<VerifyAuthResult>(
        kTestFFIErrorCodeDeserializeError);
  } catch (std::exception const &e) {
    log_error("error during verify_signed_transaction_auth: {}", e);
    return make_ffi_result_type<VerifyAuthResult>(
        kTestFFIErrorCodeUnrecoverableError);
  }
}

VerifyAuthResultFromDBResult
verify_signed_transaction_auth_from_db_impl(
    void *db, FFISerializedSignedTransaction signed_tx) noexcept
{
  constexpr auto kNullptrError =
      make_ffi_result_type<VerifyAuthResultFromDBResult>(
          kTestFFIErrorCodeInvalidPointer);

  VerifyAuthResultFromDBResult error_result;
  auto stx_opt = deserialize_signed_transaction(signed_tx, &error_result);
  if (!stx_opt) {
    return error_result;
  }
  const SignedTransaction &stx = *stx_opt;

  // Well-formed check only needs the signed transaction
  // For lightweight and full verification, we need a database
  if (!db) {
    return kNullptrError;
  }

  try {
    auto *db_context = reinterpret_cast<DBContext *>(db);

    auto err_code = db_context->run_auth_checks(stx);
    return {.auth_error_code = std::to_underlying(err_code),
            .ffi_error_code = kTestFFIErrorCodeNone};

  } catch (std::exception const &e) {
    log_error("error during verify_signed_transaction_auth_from_db: {}", e);
    return make_ffi_result_type<VerifyAuthResultFromDBResult>(
        kTestFFIErrorCodeUnrecoverableError);
  }
}

VerifyContractResult
do_verify_contract_impl(void *context, const uint8_t *contract_bytes,
                        uint32_t contract_len) noexcept
{
  if (context == nullptr || contract_bytes == nullptr) {
    return VerifyContractResult{.success = 0,
                                .ffi_error_code =
                                    kTestFFIErrorCodeInvalidPointer};
  }

  try {
    auto *rc_ptr = static_cast<std::shared_ptr<MockRuntimeContext> *>(context);
    CHECK(*rc_ptr != nullptr);
    auto *rc = rc_ptr->get();

    Contract contract;
    x23::deserialize(&contract, x23::BufView{contract_bytes, contract_len});

    auto result = rc->verify_contract(contract);
    if (result == InvokeErrorCode::Unrecoverable) {
      return VerifyContractResult{.success = 0,
                                  .ffi_error_code =
                                      kTestFFIErrorCodeUnrecoverableError};
    }
    return VerifyContractResult{.success = result == InvokeErrorCode::None
                                               ? static_cast<uint8_t>(1)
                                               : static_cast<uint8_t>(0),
                                .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (x23::XdrException const &e) {
    log_error("error deserializing contract in do_verify_contract: {}", e);
    return VerifyContractResult{.success = 0,
                                .ffi_error_code =
                                    kTestFFIErrorCodeDeserializeError};
  } catch (std::exception const &e) {
    log_error("error in do_verify_contract: {}", e);
    return VerifyContractResult{.success = 0,
                                .ffi_error_code =
                                    kTestFFIErrorCodeUnrecoverableError};
  }
}

} // namespace

} // namespace hogd

#ifdef __cplusplus
extern "C" {
#endif
#pragma GCC visibility push(default)

CreateDBResult
create_db(const uint8_t *serialized_cost_model,
          uint32_t serialized_cost_model_len)
{
  hogd::do_ffi_trace("FFI (start): create_db(serialized_cost_model={}, "
                     "serialized_cost_model_len={})",
                     static_cast<const void *>(serialized_cost_model),
                     serialized_cost_model_len);
  auto result =
      hogd::make_new_db(serialized_cost_model, serialized_cost_model_len);
  hogd::do_ffi_trace("FFI (end): create_db(serialized_cost_model={}, "
                     "serialized_cost_model_len={}) -> {}",
                     static_cast<const void *>(serialized_cost_model),
                     serialized_cost_model_len, result);
  return result;
}

GetCostModelResult
get_cost_model(void *db)
{
  hogd::do_ffi_trace("FFI (start): get_cost_model(db={})", db);
  auto result = hogd::get_cost_model(db);
  hogd::do_ffi_trace("FFI (end): get_cost_model(db={}) -> {}", db, result);
  return result;
}

GetSnapshotStorageItemResult
get_snapshot_storage_item(void *db, FFISerializedAddressAndKey addrkey)
{
  hogd::do_ffi_trace("FFI (start): get_snapshot_storage_item(db={}, "
                     "addrkey.data={}, addrkey.len={})",
                     db, static_cast<const void *>(addrkey.data), addrkey.len);
  auto result = hogd::get_snapshot_storage_item(db, addrkey);
  hogd::do_ffi_trace("FFI (end): get_snapshot_storage_item(db={}, "
                     "addrkey.data={}, addrkey.len={}) -> "
                     "ffi_error_code={}, data={}, len={}",
                     db, static_cast<const void *>(addrkey.data), addrkey.len,
                     static_cast<int>(result.ffi_error_code),
                     static_cast<const void *>(result.data.data),
                     result.data.len);
  return result;
}

TestFFIErrorCode
free_db(void *ptr)
{
  hogd::do_ffi_trace("FFI (start): free_db(ptr={})", ptr);
  auto result = hogd::free_db(static_cast<hogd::DBContext *>(ptr));
  hogd::do_ffi_trace("FFI (end): free_db(ptr={}) -> {}", ptr, result);
  return result;
}

TestFFIErrorCode
advance_new_block(void *db)
{
  hogd::do_ffi_trace("FFI (start): advance_new_block(db={})", db);
  auto result = hogd::advance_new_block(db);
  hogd::do_ffi_trace("FFI (end): advance_new_block(db={}) -> {}", db, result);
  return result;
}

OpenRuntimeContextResult
open_runtime_context()
{
  hogd::do_ffi_trace("FFI (start): open_runtime_context()");
  auto result = hogd::open_runtime_context();
  hogd::do_ffi_trace("FFI (end): open_runtime_context() -> {}", result);
  return result;
}

TestFFIErrorCode
close_runtime_context(void *context)
{
  hogd::do_ffi_trace("FFI (start): close_runtime_context(context={})", context);
  auto result = hogd::close_runtime_context(context);
  hogd::do_ffi_trace("FFI (end): close_runtime_context(context={}) -> {}",
                     context, result);
  return result;
}

TestFFIErrorCode
register_new_contract(void *context, int32_t mock_contract_id,
                      ForeignEntryPoint entry_point, void *foreign_context)
{
  hogd::do_ffi_trace(
      "FFI (start): register_new_contract(context={}, mock_contract_id={}, "
      "entry_point={}, foreign_context={})",
      context, mock_contract_id, reinterpret_cast<void *>(entry_point),
      foreign_context);
  auto result = hogd::register_new_contract(context, mock_contract_id,
                                            entry_point, foreign_context);
  hogd::do_ffi_trace(
      "FFI (end): register_new_contract(context={}, mock_contract_id={}, "
      "entry_point={}, foreign_context={}) -> {}",
      context, mock_contract_id, reinterpret_cast<void *>(entry_point),
      foreign_context, result);
  return result;
}

TestFFIErrorCode
setup_create_contract(void *context, void *db, uint8_t const *contract_bytes,
                      uint32_t contract_len)
{
  hogd::do_ffi_trace("FFI (start): setup_create_contract(context={}, db={}, "
                     "contract_bytes={}, contract_len={})",
                     context, db, static_cast<const void *>(contract_bytes),
                     contract_len);
  auto result =
      hogd::setup_create_contract(context, db, contract_bytes, contract_len);
  hogd::do_ffi_trace("FFI (end): setup_create_contract(context={}, db={}, "
                     "contract_bytes={}, contract_len={}) -> {}",
                     context, db, static_cast<const void *>(contract_bytes),
                     contract_len, result);
  return result;
}

TestFFIErrorCode
setup_deploy_contract(void *db, const uint8_t *deployment_addr,
                      const uint8_t *contract_hash, uint32_t contract_size)
{
  hogd::do_ffi_trace(
      "FFI (start): setup_deploy_contract(db={}, deployment_addr={}, "
      "contract_hash={}, contract_size={})",
      db, static_cast<const void *>(deployment_addr),
      static_cast<const void *>(contract_hash), contract_size);
  auto result = hogd::setup_deploy_contract(db, deployment_addr, contract_hash,
                                            contract_size);
  hogd::do_ffi_trace(
      "FFI (end): setup_deploy_contract(db={}, deployment_addr={}, "
      "contract_hash={}, contract_size={}) -> {}",
      db, static_cast<const void *>(deployment_addr),
      static_cast<const void *>(contract_hash), contract_size, result);
  return result;
}

OpenSyscallHandlerResult
open_syscall_handler(void *db, void *rc,
                     FFISerializedSignedTransaction signed_tx)
{
  hogd::do_ffi_trace("FFI (start): open_syscall_handler(db={}, rc={}, "
                     "signed_tx={}, signed_tx_len={})",
                     db, rc, static_cast<const void *>(signed_tx.data),
                     signed_tx.len);
  auto result = hogd::open_syscall_handler(db, rc, signed_tx);
  hogd::do_ffi_trace("FFI (end): open_syscall_handler(db={}, rc={}, "
                     "signed_tx={}, signed_tx_len={}) -> {}",
                     db, rc, static_cast<const void *>(signed_tx.data),
                     signed_tx.len, result);
  return result;
}

OpenSyscallHandlerResult
open_mocked_syscall_handler(void *db, void *rc,
                            struct FFISerializedAuthClaimData auth_claim_data,
                            const uint8_t source_tx_hash[32],
                            uint8_t enable_tracing)
{
  hogd::do_ffi_trace(
      "FFI (start): open_mocked_syscall_handler(db={}, rc={}, "
      "auth_claim_data=({}, len={}), source_tx_hash={}, enable_tracing={})",
      db, rc, static_cast<const void *>(auth_claim_data.data),
      auth_claim_data.len, static_cast<const void *>(source_tx_hash),
      enable_tracing);
  auto result =
      hogd::open_mocked_syscall_handler(db, rc, auth_claim_data, source_tx_hash,
                                        enable_tracing);
  hogd::do_ffi_trace("FFI (end): open_mocked_syscall_handler(db={}, rc={}, "
                     "auth_claim_data=({}, len={}), source_tx_hash={}, "
                     "enable_tracing={}) -> {}",
                     db, rc, static_cast<const void *>(auth_claim_data.data),
                     auth_claim_data.len,
                     static_cast<const void *>(source_tx_hash), enable_tracing,
                     result);
  return result;
}

TestFFIErrorCode
setup_grant_contract_storage_tokens(void *db, const uint8_t *contract_addr,
                                    int64_t tokens)
{
  hogd::do_ffi_trace("FFI (start): setup_grant_contract_storage_tokens(db={}, "
                     "contract_addr={}, tokens={})",
                     db, static_cast<const void *>(contract_addr), tokens);
  auto result =
      hogd::setup_grant_contract_storage_tokens(db, contract_addr, tokens);
  hogd::do_ffi_trace("FFI (end): setup_grant_contract_storage_tokens(db={}, "
                     "contract_addr={}, tokens={}) -> {}",
                     db, static_cast<const void *>(contract_addr), tokens,
                     result);
  return result;
}

TestFFIErrorCode
open_mock_invocation_frame(void *syscall_handler,
                           struct FFISerializedInvocation invocation,
                           int32_t invoke_flags)
{
  hogd::do_ffi_trace(
      "FFI (start): open_mock_invocation_frame(syscall_handler={}, "
      "invocation=({}, len={}), invoke_flags={})",
      syscall_handler, static_cast<const void *>(invocation.data),
      invocation.len, invoke_flags);
  auto result = hogd::open_mock_invocation_frame(syscall_handler, invocation,
                                                 invoke_flags);
  hogd::do_ffi_trace(
      "FFI (end): open_mock_invocation_frame(syscall_handler={}, "
      "invocation=({}, len={}), invoke_flags={}) -> {}",
      syscall_handler, static_cast<const void *>(invocation.data),
      invocation.len, invoke_flags, result);
  return result;
}

ForeignTracedInvokeResult
finish_mock_invocation_frame(void *syscall_handler, int32_t invoke_error_code)
{
  hogd::do_ffi_trace(
      "FFI (start): finish_mock_invocation_frame(syscall_handler={}, "
      "invoke_error_code={})",
      syscall_handler, invoke_error_code);
  auto result =
      hogd::finish_mock_invocation_frame(syscall_handler, invoke_error_code);
  hogd::do_ffi_trace(
      "FFI (end): finish_mock_invocation_frame(syscall_handler={}, "
      "invoke_error_code={}) -> {}",
      syscall_handler, invoke_error_code, result);
  return result;
}

TestFFIErrorCode
push_context(void *syscall_handler)
{
  hogd::do_ffi_trace("FFI (start): push_context(syscall_handler={})",
                     syscall_handler);
  auto result = hogd::push_context(syscall_handler);
  hogd::do_ffi_trace("FFI (end): push_context(syscall_handler={}) -> {}",
                     syscall_handler, result);
  return result;
}

TestFFIErrorCode
pop_context()
{
  hogd::do_ffi_trace("FFI (start): pop_context()");
  auto result = hogd::pop_context();
  hogd::do_ffi_trace("FFI (end): pop_context() -> {}", result);
  return result;
}

TestFFIErrorCode
close_syscall_handler(void *syscall_handler)
{
  hogd::do_ffi_trace("FFI (start): close_syscall_handler(syscall_handler={})",
                     syscall_handler);
  auto result = hogd::close_syscall_handler(syscall_handler);
  hogd::do_ffi_trace(
      "FFI (end): close_syscall_handler(syscall_handler={}) -> {}",
      syscall_handler, result);
  return result;
}

struct ForeignTracedInvokeResult
root_invoke(void *syscall_handler, uint8_t enable_tracing)
{
  hogd::do_ffi_trace(
      "FFI (start): root_invoke(syscall_handler={}, enable_tracing={})",
      syscall_handler, enable_tracing);
  auto result = hogd::root_invoke(syscall_handler, enable_tracing == 1);
  hogd::do_ffi_trace(
      "FFI (end): root_invoke(syscall_handler={}, enable_tracing={}) -> {}",
      syscall_handler, enable_tracing, result);
  return result;
}

struct ForeignTryCommitResult
try_commit_transaction(void *syscall_handler)
{
  hogd::do_ffi_trace("FFI (start): try_commit_transaction(syscall_handler={})",
                     syscall_handler);
  auto result = hogd::try_commit_transaction(syscall_handler);
  hogd::do_ffi_trace(
      "FFI (end): try_commit_transaction(syscall_handler={}) -> {}",
      syscall_handler, result);
  return result;
}

TestFFIErrorCode
abort_transaction(void *syscall_handler)
{
  hogd::do_ffi_trace("FFI (start): abort_transaction(syscall_handler={})",
                     syscall_handler);
  auto result = hogd::abort_transaction(syscall_handler);
  hogd::do_ffi_trace("FFI (end): abort_transaction(syscall_handler={}) -> {}",
                     syscall_handler, result);
  return result;
}

struct ForeignHostFnResult
mock_syscall_entrypoint(int32_t syscall, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5,
                        uint64_t arg6, uint64_t arg7)
{
  hogd::do_ffi_trace(
      "FFI (start): mock_syscall_entrypoint(syscall={}, arg1={}, arg2={}, "
      "arg3={}, arg4={}, arg5={}, arg6={}, arg7={})",
      syscall, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
  // First-use init can fail (allocation failure); translate it into an
  // unrecoverable host-function result rather than letting it escape this C ABI
  // boundary. lazy_init() reports the failure to stderr.
  if (!hogd::lazy_init()) {
    return hogd::unrecoverable_error();
  }
  auto result = hogd::mock_syscall_entrypoint(syscall, arg1, arg2, arg3, arg4,
                                              arg5, arg6, arg7);
  hogd::do_ffi_trace("FFI (end): mock_syscall_entrypoint(syscall={}) -> {}",
                     syscall, result);
  return result;
}

void
enable_syscall_logging(int32_t level)
{
  hogd::do_ffi_trace("FFI (start): enable_syscall_logging(level={})", level);
  hogd::enable_syscall_logging(level);
  hogd::do_ffi_trace("FFI (end): enable_syscall_logging(level={})", level);
}

VerifyAuthResult
verify_signed_transaction_auth(FFISerializedSignedTransaction signed_tx,
                               FFISourceAuthData source_auth_data)
{
  hogd::do_ffi_trace(
      "FFI (start): verify_signed_transaction_auth(signed_tx={}, "
      "source_auth_data={})",
      signed_tx, source_auth_data);
  // Install a logger before entering auth verification: an invalid signature
  // logs a debug-build trace() that would otherwise dereference a null logger
  // when this is the first test_ffi call in the process (issue #2464).
  // First-use init can fail (allocation failure); translate it into a
  // structured error rather than letting it escape this C ABI boundary, which
  // runs before the impl's catch boundary (issue #2514). lazy_init() reports
  // the failure to stderr.
  if (!hogd::lazy_init()) {
    return VerifyAuthResult{.auth_error_code = 0,
                            .ffi_error_code =
                                kTestFFIErrorCodeUnrecoverableError};
  }
  auto result =
      hogd::verify_signed_transaction_auth_impl(signed_tx, source_auth_data);
  hogd::do_ffi_trace("FFI (end): verify_signed_transaction_auth(signed_tx={}, "
                     "source_auth_data={}) -> {}",
                     signed_tx, source_auth_data, result);
  return result;
}

VerifyAuthResultFromDBResult
verify_signed_transaction_auth_from_db(void *db,
                                       FFISerializedSignedTransaction signed_tx)
{
  hogd::do_ffi_trace(
      "FFI (start): verify_signed_transaction_auth_from_db(db={}, "
      "signed_tx={}, signed_tx_len={})",
      db, static_cast<const void *>(signed_tx.data), signed_tx.len);
  auto result =
      hogd::verify_signed_transaction_auth_from_db_impl(db, signed_tx);
  hogd::do_ffi_trace("FFI (end): verify_signed_transaction_auth_from_db(db={}, "
                     "signed_tx={}, signed_tx_len={}) -> {}",
                     db, static_cast<const void *>(signed_tx.data),
                     signed_tx.len, result);
  return result;
}

VerifyContractResult
do_verify_contract(void *context, const uint8_t *contract_bytes,
                   uint32_t contract_len)
{
  hogd::do_ffi_trace(
      "FFI (start): do_verify_contract(context={}, contract_bytes={}, "
      "contract_len={})",
      context, static_cast<const void *>(contract_bytes), contract_len);
  auto result =
      hogd::do_verify_contract_impl(context, contract_bytes, contract_len);
  hogd::do_ffi_trace(
      "FFI (end): do_verify_contract(context={}, contract_bytes={}, "
      "contract_len={}) -> success={}, ffi_error={}",
      context, static_cast<const void *>(contract_bytes), contract_len,
      result.success, result.ffi_error_code);
  return result;
}

TestFFIErrorCode
free_ffi_data_buffer(struct FFIDataBuffer buffer)
{
  hogd::do_ffi_trace("FFI (start): free_ffi_data_buffer(buffer={})", buffer);
  auto result = hogd::free_ffi_data_buffer(buffer);
  // Log pointer address and length only — buffer.data has been freed, so the
  // FFIDataBuffer formatter must not dereference it.
  hogd::do_ffi_trace(
      "FFI (end): free_ffi_data_buffer(data=<freed>, len={}) -> {}", buffer.len,
      result);
  return result;
}

#pragma GCC visibility pop
#ifdef __cplusplus
}
#endif
