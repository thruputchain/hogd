#include "hogd/test_ffi/test_bindings.h"

#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/auth_claim/status.h"
#include "hogd/auth_claim/verify.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/test_ffi/db_context_wrapper.h"
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
make_new_db(const uint8_t *serialized_cost_model,
            uint32_t serialized_cost_model_len) noexcept
{
  lazy_init();
  try {
    StorageCostModelXdr cost_model_xdr =
        StorageCostModel::default_cost_model().to_xdr();
    if (serialized_cost_model != nullptr) {
      try {
        x23::deserialize(&cost_model_xdr,
                         x23::BufView{serialized_cost_model,
                                      serialized_cost_model_len});
      } catch (x23::XdrBadInput const &e) {
        log_error("error deserializing cost model: {}", e);
        return {.db = nullptr,
                .ffi_error_code = kTestFFIErrorCodeDeserializeError};
      }
    }
    auto *db = new DBContext(cost_model_xdr);
    ffi_trace("FFI: make_new_db() -> (db={})", static_cast<void *>(db));
    return {.db = db, .ffi_error_code = kTestFFIErrorCodeNone};
  } catch (std::exception const &e) {
    log_error("error creating db context: {}", e);
    return {.db = nullptr,
            .ffi_error_code = kTestFFIErrorCodeUnrecoverableError};
  }
}

constexpr FFIDataBuffer kNullBuffer = FFIDataBuffer{.data = nullptr, .len = 0};

std::expected<FFIDataBuffer, TestFFIErrorCode>
make_new_data_buffer(uint32_t len) noexcept
{
  if (len == 0) {
    return FFIDataBuffer{.data = nullptr, .len = 0};
  }
  try {
    auto *data = new uint8_t[len];
    return FFIDataBuffer{.data = data, .len = len};
  } catch (std::exception const &e) {
    log_error("error allocating data buffer: {}", e);
    return std::unexpected(kTestFFIErrorCodeUnrecoverableError);
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

  auto cost_model = db_context->get_base_db().get_cost_model().to_xdr();

  try {
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

TestFFIErrorCode
free_ffi_data_buffer_impl(FFIDataBuffer buffer) noexcept
{
  if (buffer.data == nullptr && buffer.len != 0) {
    return kTestFFIErrorCodeInvalidPointer;
  }
  delete[] buffer.data;
  return kTestFFIErrorCodeNone;
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
open_syscall_handler(void *db, void *rc,
                     FFISerializedSignedTransaction signed_tx) noexcept
{
  constexpr auto kNullptrError = make_ffi_result_type<OpenSyscallHandlerResult>(
      kTestFFIErrorCodeInvalidPointer);
  ffi_trace(
      "FFI: open_syscall_handler(db={}, rc={}, signed_tx={}, signed_tx_len={})",
      db, rc, static_cast<const void *>(signed_tx.data), signed_tx.len);

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
        new SyscallHandlerContext(runtime_context->get(), stx, *db_context));
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
root_invoke(void *syscall_handler, bool enable_tracing) noexcept
{
  if (!syscall_handler) {
    error("root_invoke error: no syscall handler!");
    return make_ffi_result_type<ForeignRootInvokeResult>(
        kTestFFIErrorCodeInvalidPointer);
  }

  try {
    auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
    if (enable_tracing) {
      context->to_handler().enable_tracing();
    }
    auto res = context->root_invoke();
    if (enable_tracing) {
      auto const *trace = context->to_handler().get_trace();
      if (trace) {
        auto serialized_trace = x23::serialize(*trace);
        auto buffer = make_new_data_buffer(serialized_trace.size());
        if (!buffer.has_value()) {
          error("failed to allocate memory for serialized trace!");
          return make_ffi_result_type<ForeignRootInvokeResult>(
              kTestFFIErrorCodeUnrecoverableError);
        }
        std::memcpy(buffer.value().data, serialized_trace.data(),
                    serialized_trace.size());
        res.serialized_trace = buffer.value();
      }
    }
    ffi_trace("FFI: root_invoke(syscall_handler={}) -> ({})", syscall_handler,
              format_foreign_root_invoke_result(res));
    return res;
  } catch (std::exception const &e) {
    log_error("unhandled error during root_invoke: {}", e);
    return kUnrecoverableError;
  }
}

std::string
format_foreign_try_commit_result(const ForeignTryCommitResult &result)
{
  return std::format("result_code={}, ffi_error_code={}", result.result_code,
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
    if (res == ExecResult::ExecResultOK) {
      auto events_res = context->to_handler().emit_events()();
      if (!events_res.has_value()) {
        error("failed to emit events: {}", events_res.error());
        return ForeignTryCommitResult{
            .result_code = std::to_underlying(
                SystemCallHandler::to_exec_result(events_res.error().code()))};
      }
    }
    auto out = ForeignTryCommitResult{.result_code = std::to_underlying(res)};
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

constexpr StorageCostModel kCostModel(12000, 1, 5, 100, 5, 1234);

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
    trace("granting {} storage tokens for address {}", tokens, x23::show(addr));
    db_cast->get_base_db().grant_storage_token(addr, tokens);
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

  auto &dao = db_cast->get_base_db().get_db();
  auto res = dao.read_wasm_code(h);
  if (!res) {
    error(
        "Attempt to deploy contract that has not been created in the database");
    return kTestFFIErrorCodeDeployNonexistentContract;
  }
  Contract contract;
  try {
    x23::deserialize(&contract,
                     x23::BufView{res.value().data(), res.value().size()});
  } catch (x23::XdrException &e) {
    log_error("unrecoverable: error deserializing contract from db: {}", e);
    return kTestFFIErrorCodeUnrecoverableError;
  }

  db_cast->get_base_db().write_deployment(
      addr,
      DeployedContractHash{.hash = h,
                           .contract_size = contract_size,
                           .contract_type = contract.contract_type()},
      &kCostModel);

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
  } catch (x23::XdrBadInput const &e) {
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
                                    FFISourceAuthData source_auth_data,
                                    FFIAuxiliaryAuthData aux_auth_data) noexcept
{
  constexpr auto kNullptrError =
      make_ffi_result_type<VerifyAuthResult>(kTestFFIErrorCodeInvalidPointer);

  ffi_trace("FFI: verify_signed_transaction_auth(signed_tx={}, "
            "source_auth_data={}, aux_auth_data={}, count={})",
            static_cast<const void *>(signed_tx.data),
            static_cast<const void *>(source_auth_data.data),
            static_cast<const void *>(aux_auth_data.auth_data),
            aux_auth_data.count);

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

    // Deserialize auxiliary auth data
    std::unordered_map<ContractAddress, const AddressAuthData *> aux_data_map;
    std::vector<std::unique_ptr<AddressAuthData>> aux_data_storage;

    if (aux_auth_data.count > 0) {
      if (!aux_auth_data.auth_data) {
        return kNullptrError;
      }

      for (uint32_t i = 0; i < aux_auth_data.count; i++) {
        ContractAddress addr;
        std::memcpy(addr.data(), aux_auth_data.auth_data[i].address,
                    addr.size());

        auto aux_data = std::make_unique<AddressAuthData>();
        x23::deserialize(aux_data.get(),
                         x23::BufView{aux_auth_data.auth_data[i].data,
                                      aux_auth_data.auth_data[i].len});

        aux_data_map[addr] = aux_data.get();
        aux_data_storage.push_back(std::move(aux_data));
      }
    }

    // Perform the appropriate level of verification
    AuthVerifyStatus<void> result = verify_auth(stx, source_data, aux_data_map);

    if (result.has_value()) {
      return {.auth_error_code = 0, .ffi_error_code = kTestFFIErrorCodeNone};
    }
    return {.auth_error_code = std::to_underlying(result.error().code()),
            .ffi_error_code = kTestFFIErrorCodeNone};

  } catch (x23::XdrBadInput const &e) {
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

  ffi_trace("FFI: verify_signed_transaction_auth_from_db(db={}, signed_tx={}, "
            "signed_tx_len={})",
            db, static_cast<const void *>(signed_tx.data), signed_tx.len);

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

} // namespace

} // namespace hogd

#ifdef __cplusplus
extern "C" {
#endif

CreateDBResult
create_db(const uint8_t *serialized_cost_model,
          uint32_t serialized_cost_model_len)
{
  return hogd::make_new_db(serialized_cost_model, serialized_cost_model_len);
}

GetCostModelResult
get_cost_model(void *db)
{
  return hogd::get_cost_model(db);
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
open_syscall_handler(void *db, void *rc,
                     FFISerializedSignedTransaction signed_tx)
{
  return hogd::open_syscall_handler(db, rc, signed_tx);
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
root_invoke(void *syscall_handler, uint8_t enable_tracing)
{
  return hogd::root_invoke(syscall_handler, enable_tracing == 1);
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

VerifyAuthResult
verify_signed_transaction_auth(FFISerializedSignedTransaction signed_tx,
                               FFISourceAuthData source_auth_data,
                               FFIAuxiliaryAuthData aux_auth_data)
{
  return hogd::verify_signed_transaction_auth_impl(signed_tx, source_auth_data,
                                                   aux_auth_data);
}

VerifyAuthResultFromDBResult
verify_signed_transaction_auth_from_db(void *db,
                                       FFISerializedSignedTransaction signed_tx)
{
  return hogd::verify_signed_transaction_auth_from_db_impl(db, signed_tx);
}

TestFFIErrorCode
free_ffi_data_buffer(struct FFIDataBuffer buffer)
{
  return hogd::free_ffi_data_buffer_impl(buffer);
}

#ifdef __cplusplus
}
#endif
