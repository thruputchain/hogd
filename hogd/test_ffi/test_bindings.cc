#include "hogd/test_ffi/test_bindings.h"

#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "account.hh"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/logger.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/mock_syscall_entry.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/debug.h"
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

// error("message {}", e.what()); can throw an allocation failure;
// This function works around that issue.
void
log_error(std::format_string<const char *> message, std::exception const &e)
{
  try {
    error(message, e.what());
  } catch (...) {
    error("Failed to print error message!");
  }
}

ThreadsafeLogger *global_logger = nullptr;
x23::DebugFd<"TEST_FFI_TRACE"> ffi_trace;

void
lazy_init()
{
  static std::mutex init_mutex;
  std::lock_guard lock(init_mutex);
  static bool initialized = false;
  ffi_trace("lazy_init: initialized={}", initialized);
  if (!initialized) {
    hogd_init(0, nullptr, false);
    auto logger = std::make_unique<ThreadsafeLogger>();
    global_logger = logger.get();
    hogd::swap_logger(std::move(logger));
    initialized = true;
  }
}

void
enable_syscall_logging() noexcept
{
  ffi_trace("FFI: enable_syscall_logging() -> ()");
  lazy_init();
  assert(global_logger != nullptr);
  global_logger->set_log_level(LogLevel::kTrace);
}

CreateDBResult
make_new_db() noexcept
{
  lazy_init();
  try {
    auto *db = new DBContext();
    ffi_trace("FFI: make_new_db() -> (db={})", static_cast<void *>(db));
    return {.db = db, .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (std::exception const &e) {
    log_error("error creating db context: {}", e);
    return {.db = nullptr,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
}

TestFFIErrorCode
free_db(DBContext *ptr) noexcept
{
  try {
    ffi_trace("FFI: free_db(db={}) -> ()", static_cast<void *>(ptr));
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
  ffi_trace("FFI: advance_new_block(db={}) -> ()", db);
  auto *ptr = reinterpret_cast<DBContext *>(db);
  if (!ptr)
    return kTestFFIErrorCodeInvalidPointer;

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
  lazy_init();
  std::shared_ptr<hogd::MockRuntimeContext> *out;
  try {
    out = new std::shared_ptr<hogd::MockRuntimeContext>();
    *out = std::make_shared<hogd::MockRuntimeContext>(make_hogexec_context());
  } catch (std::exception const &e) {
    log_error("error creating runtime context: {}", e);
    return {.context = nullptr,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
  CHECK(*out != nullptr);
  ffi_trace("FFI: open_runtime_context() -> (context={})",
            static_cast<void *>(out));
  return {.context = out, .ffi_error_code = kTestFFIErrorCodeNone};
}

TestFFIErrorCode
close_runtime_context(void *context) noexcept
{
  try {
    ffi_trace("FFI: close_runtime_context(context={}) -> ()", context);
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
  ffi_trace("FFI: register_new_contract(context={}, mock_contract_id={}, "
            "entry_point={}, foreign_context={})",
            context, mock_contract_id, reinterpret_cast<void *>(entry_point),
            foreign_context);
  if (context == nullptr || entry_point == nullptr)
    return kTestFFIErrorCodeInvalidPointer;

  auto *c = static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);
  if (!c)
    return kTestFFIErrorCodeInvalidPointer;
  CHECK(*c != nullptr);

  (*c)->set_trampoline(mock_contract_id, entry_point, foreign_context);
  return kTestFFIErrorCodeNone;
}

OpenSyscallHandlerResult
open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx,
                     unsigned int signed_tx_len) noexcept
{
  constexpr auto kNullptrError = make_ffi_result_type<OpenSyscallHandlerResult>(
      kTestFFIErrorCodeInvalidPointer);
  ffi_trace(
      "FFI: open_syscall_handler(db={}, rc={}, signed_tx={}, signed_tx_len={})",
      db, rc, static_cast<const void *>(signed_tx), signed_tx_len);

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

  if (!signed_tx) {
    return kNullptrError;
  }
  try {
    hogd::SignedTransaction stx;
    x23::deserialize(&stx, x23::BufView{signed_tx, signed_tx_len});
    auto *out = static_cast<void *>(
        new SyscallHandlerContext(*runtime_context, stx, *db_context));
    return {.syscall_handler = out, .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (x23::XdrBadInput const &e) {
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
  try {
    ffi_trace("FFI: close_syscall_handler(syscall_handler={}) -> ()",
              syscall_handler);
    delete static_cast<SyscallHandlerContext *>(syscall_handler);
    return kTestFFIErrorCodeNone;
  } catch (std::exception const &e) {
    log_error("error closing syscall handler: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
}

constexpr ForeignRootInvokeResult kUnrecoverableError = {
    .result = {.panic =
                   std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)},
    .gas_consumed = 0};

std::string
format_foreign_root_invoke_result(const ForeignRootInvokeResult &result)
{
  return std::format("result={{.panic={}}}, gas_consumed={}, ffi_error_code={}",
                     result.result.panic, result.gas_consumed,
                     std::to_underlying(result.ffi_error_code));
}

ForeignRootInvokeResult
root_invoke(void *syscall_handler) noexcept
{
  if (!syscall_handler) {
    error("root_invoke error: no syscall handler!");
    return make_ffi_result_type<ForeignRootInvokeResult>(
        kTestFFIErrorCodeInvalidPointer);
  }

  try {
    auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
    auto res = context->root_invoke();
    ffi_trace("FFI: root_invoke(syscall_handler={}) -> (result={})",
              syscall_handler, format_foreign_root_invoke_result(res));
    return res;
  } catch (std::exception const &e) {
    log_error("unhandled error during root_invoke: {}", e);
    return kUnrecoverableError;
  }
}

std::string
format_foreign_try_commit_result(const ForeignTryCommitResult &result)
{
  return std::format("result_code={}, panic={}, ffi_error_code={}",
                     result.result_code, result.panic,
                     std::to_underlying(result.ffi_error_code));
}

ForeignTryCommitResult
try_commit_transaction(void *syscall_handler) noexcept
{
  constexpr auto kUnrecoverableError =
      make_ffi_result_type<ForeignTryCommitResult>(
          kTestFFIErrorCodeUnrecoverableError);

  if (!syscall_handler) {
    error("try_commit_transaction error: no syscall handler!");
    return make_ffi_result_type<ForeignTryCommitResult>(
        kTestFFIErrorCodeInvalidPointer);
  }

  try {
    auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
    auto res = context->to_handler().try_commit_transaction();
    context->mark_committed_or_aborted();
    if (!res.has_value()) {
      error("try_commit_transaction encountered system error {}", res.error());
      return ForeignTryCommitResult{.result_code = 0,
                                    .panic =
                                        std::to_underlying(res.error().code())};
    }
    auto out = ForeignTryCommitResult{.result_code = *res ? 1 : 0, .panic = 0};
    ffi_trace("FFI: try_commit_transaction(syscall_handler={}) -> (result={})",
              syscall_handler, format_foreign_try_commit_result(out));
    return out;
  } catch (std::exception const &e) {
    log_error("error during try_commit_transaction: {}", e);
    return kUnrecoverableError;
  }
}

TestFFIErrorCode
abort_transaction(void *syscall_handler) noexcept
{
  if (!syscall_handler) {
    error("abort_transaction error: no syscall handler!");
    return kTestFFIErrorCodeInvalidPointer;
  }
  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
  auto res = context->to_handler().abort_transaction();
  context->mark_committed_or_aborted();
  ffi_trace("FFI: abort_transaction(syscall_handler={}) -> ()",
            syscall_handler);
  if (!res.has_value()) {
    try {
      error("failed to abort transaction: {}", res.error());
      return kTestFFIErrorCodeUnrecoverableError;
    } catch (std::exception const &e) {
      log_error("error during abort_transaction: {}", e);
      return kTestFFIErrorCodeUnrecoverableError;
    }
  }
  return kTestFFIErrorCodeNone;
}

constexpr StorageCostModel kCostModel(12000, 1, 5, 100, 5);

TestFFIErrorCode
setup_grant_contract_storage_tokens(void *db, const uint8_t *contract_addr,
                                    int64_t tokens) noexcept
{
  ffi_trace("FFI: setup_grant_contract_storage_tokens(db={}, contract_addr={}, "
            "tokens={})",
            db, static_cast<const void *>(contract_addr), tokens);
  if (db == nullptr || contract_addr == nullptr) {
    return kTestFFIErrorCodeInvalidPointer;
  }
  auto *db_cast = static_cast<DBContext *>(db);
  hogd::ContractAddress addr;
  std::memcpy(addr.data(), contract_addr, addr.size());

  try {
    db_cast->get_base_db().write_item(
        AddressAndKey{
            .address = addr,
            .key = StorageKey(
                StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance),
        },
        std::make_shared<StorageItemWrapper>(int128_t{tokens}, &kCostModel));
  } catch (std::exception const &e) {
    log_error("error granting storage tokens: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }

  return kTestFFIErrorCodeNone;
}

TestFFIErrorCode
get_event_log(void *syscall_handler, unsigned char *buffer, uint32_t buffer_len)
{
  ffi_trace("FFI: get_event_log(syscall_handler={}, buffer={}, buffer_len={})",
            syscall_handler, static_cast<const void *>(buffer), buffer_len);
  if (!syscall_handler) {
    error("get_event_log error: no syscall handler!");
    return kTestFFIErrorCodeInvalidPointer;
  }
  if (!buffer) {
    error("get_event_log error: no buffer!");
    return kTestFFIErrorCodeInvalidPointer;
  }

  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
  auto event_log = context->get_tx_event_log();
  if (!event_log) {
    debug("No events for current tx");
    if (buffer_len > 0) {
      return kTestFFIErrorCodeEventLogNotFound;
    }
    return kTestFFIErrorCodeNone;
  }

  try {
    auto serialized_event_log = x23::serialize(event_log.value());
    if (serialized_event_log.size() != buffer_len) {
      error("get_event_log error: buffer too small!");
      return kTestFFIErrorCodeInvalidPointer;
    }
    std::memcpy(buffer, serialized_event_log.data(),
                serialized_event_log.size());
    return kTestFFIErrorCodeNone;
  } catch (x23::XdrException &e) {
    log_error("error serializing event log: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  } catch (std::exception const &e) {
    log_error("error during get_event_log: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }
}

GetEventLogLenResult
get_event_log_len(void *syscall_handler)
{
  if (!syscall_handler) {
    error("get_event_log_len error: no syscall handler!");
    ffi_trace(
        "FFI: get_event_log_len(syscall_handler={}) -> (ffi_error_code={})",
        syscall_handler, std::to_underlying(kTestFFIErrorCodeInvalidPointer));
    return make_ffi_result_type<GetEventLogLenResult>(
        kTestFFIErrorCodeInvalidPointer);
  }
  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
  auto event_log = context->get_tx_event_log();
  if (!event_log) {
    ffi_trace(
        "FFI: get_event_log_len(syscall_handler={}) -> (ffi_error_code={})",
        syscall_handler, std::to_underlying(kTestFFIErrorCodeEventLogNotFound));
    return make_ffi_result_type<GetEventLogLenResult>(
        kTestFFIErrorCodeEventLogNotFound);
  }

  try {
    auto serialized_event_log = x23::serialize(event_log.value());
    uint32_t len = serialized_event_log.size();
    ffi_trace("FFI: get_event_log_len(syscall_handler={}) -> (len={})",
              syscall_handler, len);
    return {.len = len, .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (x23::XdrException &e) {
    log_error("error serializing event log: {}", e);
    return make_ffi_result_type<GetEventLogLenResult>(
        kTestFFIErrorCodeUnrecoverableError);
  } catch (std::exception const &e) {
    log_error("error during get_event_log_len: {}", e);
    return make_ffi_result_type<GetEventLogLenResult>(
        kTestFFIErrorCodeUnrecoverableError);
  }
}

TestFFIErrorCode
setup_create_contract(void *context, void *db, uint8_t const *contract_bytes,
                      uint32_t contract_len) noexcept
{
  ffi_trace("FFI: setup_create_contract(context={}, db={}, contract_bytes={}, "
            "contract_len={}) -> ()",
            context, db, static_cast<const void *>(contract_bytes),
            contract_len);
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
  }

  // make sure the contract has already been registered with the instantiation
  // context
  if (contract.contract_type() == ContractType::kMockTest) {
    if (!((*context_cast)->has_mock_contract(contract.mock_contract_id()))) {
      error("contract has not been registered with the instantiation context");
      return kTestFFIErrorCodeCreateNonexistentMockContract;
    }
  }

  auto h = sha256(contract);

  db_cast->get_base_db().write_contract(h, std::make_shared<Contract>(
                                               std::move(contract)));
  return kTestFFIErrorCodeNone;
}

TestFFIErrorCode
setup_deploy_contract(void *db, const uint8_t *deployment_addr,
                      const uint8_t *contract_hash,
                      uint32_t contract_size) noexcept
{
  ffi_trace("FFI: setup_deploy_contract(db={}, deployment_addr={}, "
            "contract_hash={}, contract_size={}) -> ()",
            db, static_cast<const void *>(deployment_addr),
            static_cast<const void *>(contract_hash), contract_size);

  if (db == nullptr || contract_hash == nullptr || deployment_addr == nullptr) {
    return kTestFFIErrorCodeInvalidPointer;
  }
  auto *db_cast = static_cast<DBContext *>(db);

  SHA256Hash h;
  std::memcpy(h.data(), contract_hash, h.size());
  ContractAddress addr;
  std::memcpy(addr.data(), deployment_addr, addr.size());

  // check that the contract hash has been created
  if (db_cast->get_base_db().get_post_block_contract(h) == nullptr) {
    error(
        "Attempt to deploy contract that has not been created in the database");
    return kTestFFIErrorCodeDeployNonexistentContract;
  }

  std::shared_ptr<StorageItemWrapper> item =
      std::make_shared<StorageItemWrapper>(
          DeployedContractHash{.hash = h, .contract_size = contract_size},
          &kCostModel);

  db_cast->get_base_db().write_item(
      AddressAndKey{.address = addr,
                    .key = StorageKey(
                        StorageKeyType::kStorageKeyTypeAddressMetadata)},
      std::move(item));
  return kTestFFIErrorCodeNone;
}

} // namespace

} // namespace hogd

#ifdef __cplusplus
extern "C" {
#endif

CreateDBResult
create_db()
{
  return hogd::make_new_db();
}

TestFFIErrorCode
free_db(void *ptr)
{
  return hogd::free_db(static_cast<hogd::DBContext *>(ptr));
}

TestFFIErrorCode
advance_new_block(void *db)
{
  return hogd::advance_new_block(db);
}

OpenRuntimeContextResult
open_runtime_context()
{
  return hogd::open_runtime_context();
}

TestFFIErrorCode
close_runtime_context(void *context)
{
  return hogd::close_runtime_context(context);
}

TestFFIErrorCode
register_new_contract(void *context, int32_t mock_contract_id,
                      ForeignEntryPoint entry_point, void *foreign_context)
{
  return hogd::register_new_contract(context, mock_contract_id, entry_point,
                                     foreign_context);
}

TestFFIErrorCode
setup_create_contract(void *context, void *db, uint8_t const *contract_bytes,
                      uint32_t contract_len)
{
  return hogd::setup_create_contract(context, db, contract_bytes, contract_len);
}

TestFFIErrorCode
setup_deploy_contract(void *db, const uint8_t *deployment_addr,
                      const uint8_t *contract_hash, uint32_t contract_size)
{
  return hogd::setup_deploy_contract(db, deployment_addr, contract_hash,
                                     contract_size);
}

OpenSyscallHandlerResult
open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx,
                     unsigned int signed_tx_len)
{
  return hogd::open_syscall_handler(db, rc, signed_tx, signed_tx_len);
}

TestFFIErrorCode
setup_grant_contract_storage_tokens(void *db, const uint8_t *contract_addr,
                                    int64_t tokens)
{
  return hogd::setup_grant_contract_storage_tokens(db, contract_addr, tokens);
}

TestFFIErrorCode
close_syscall_handler(void *syscall_handler)
{
  return hogd::close_syscall_handler(syscall_handler);
}

struct ForeignRootInvokeResult
root_invoke(void *syscall_handler)
{
  return hogd::root_invoke(syscall_handler);
}

struct ForeignTryCommitResult
try_commit_transaction(void *syscall_handler)
{
  return hogd::try_commit_transaction(syscall_handler);
}

TestFFIErrorCode
abort_transaction(void *syscall_handler)
{
  return hogd::abort_transaction(syscall_handler);
}

struct ForeignHostFnResult
mock_syscall_entrypoint(int32_t syscall, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5,
                        uint64_t arg6, uint64_t arg7)
{
  return hogd::mock_syscall_entrypoint(syscall, arg1, arg2, arg3, arg4, arg5,
                                       arg6, arg7);
}

void
enable_syscall_logging()
{
  hogd::enable_syscall_logging();
}

TestFFIErrorCode
get_event_log(void *syscall_handler, unsigned char *buffer, uint32_t buffer_len)
{
  return hogd::get_event_log(syscall_handler, buffer, buffer_len);
}

GetEventLogLenResult
get_event_log_len(void *syscall_handler)
{
  return hogd::get_event_log_len(syscall_handler);
}

#ifdef __cplusplus
}
#endif
