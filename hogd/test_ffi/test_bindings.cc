#include "hogd/test_ffi/test_bindings.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>

#include "coord.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/mock_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc.hh"
#include "syscall.hh"
#include "syscall_args.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {

struct PortAllocator {
  inline static std::atomic<uint32_t> port_start_ = 10000;

  static std::string allocate_port()
  {
    return std::to_string(port_start_.fetch_add(1));
  }
};

class DBContext {
public:
  using db_client_t = DatabaseToCoordInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  const std::string db_to_exec_port;
  const std::string db_to_coord_port;
  const std::string exec_to_coord_port;

  DBContext()
    : db_to_exec_port(PortAllocator::allocate_port()),
      db_to_coord_port(PortAllocator::allocate_port()),
      exec_to_coord_port(PortAllocator::allocate_port()),
      db_(1, db_to_exec_port, db_to_coord_port), pool_(1), cp_(pool_),
      db_client_([this]() -> db_client_t {
        auto fd = cp_.tcpconnect("localhost", db_to_coord_port)();
        if (!fd) {
          exit(-1);
        }

        auto client_config = x23::rpc_stream_logged(cp_, *fd);
        return client_config.template client<DatabaseToCoordInterfaceV1>();
      }()),
      db_proxy_(
          cp_, DatabaseInfo{.database_address = "localhost",
                            .database_to_coord_port = {db_to_coord_port.c_str(),
                                                       db_to_coord_port.size()},
                            .database_to_exec_port = {db_to_exec_port.c_str(),
                                                      db_to_exec_port.size()}})
  {
    db_.get_root_db().set_as_default_genesis();
  }

  void advance_new_block()
  {
    if (current_block_id_ != 0) {
      if (db_client_.close_snapshot(current_block_id_)() != RPCError::kNone) {
        exit(-1);
      }
    }
    if (db_client_.open_database_snapshot(current_block_id_,
                                          current_block_id_ + 1)() !=
        RPCError::kNone) {
      exit(-1);
    }
    syscall_logfd = 1;
    current_block_id_++;
    snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(current_block_id_, db_proxy_);
  }

  bool is_block_open() const { return current_block_id_ != 0; }

  BlockID current_block_id() const { return current_block_id_; }

  TransactionID allocate_tx_id() { return next_tx_id_++; }

  StorageSnapshotCache &get_snapshot()
  {
    assert(is_block_open());
    assert(snapshot_cache_);
    return *snapshot_cache_;
  }

  PerTransactionDatabaseProxy get_per_tx_db_proxy()
  {
    return PerTransactionDatabaseProxy(current_block_id_, allocate_tx_id(),
                                       db_proxy_);
  }

private:
  HogdbCore db_;
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
  // assumes genesis is 0
  BlockID current_block_id_ = 0;
  TransactionID next_tx_id_ = 0;

  db_client_t db_client_;

  DatabaseProxy db_proxy_;

  std::unique_ptr<StorageSnapshotCache> snapshot_cache_ = nullptr;
};

class SyscallHandlerContext {
public:
  SyscallHandlerContext(std::shared_ptr<MockRuntimeContext> context,
                        SignedTransaction const &stx, DBContext &db)
    : per_tx_db_proxy_(db.get_per_tx_db_proxy()),
      handler_(stx, db.get_snapshot(), per_tx_db_proxy_, std::nullopt,
               std::move(context))
  {}

  ForeignRootInvokeResult root_invoke()
  {
    assert(get_context() == this);

    auto res = handler_.root_invoke();

    if (res.result_) {
      return ForeignRootInvokeResult{
          .result = ForeignInvokeResult{.result_code = *res.result_,
                                        .panic = std::to_underlying(
                                            InvokeErrorCode::kNone)},
          .gas_consumed = res.gas_consumed_};
    }

    return ForeignRootInvokeResult{.result = {.result_code = 0,
                                              .panic = std::to_underlying(
                                                  res.result_.error().code())},
                                   .gas_consumed = res.gas_consumed_};
  }

  static SyscallHandlerContext *get_context() { return current_context_; }

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

private:
  inline static thread_local SyscallHandlerContext *current_context_ = nullptr;

  PerTransactionDatabaseProxy per_tx_db_proxy_;
  SystemCallHandler handler_;
};

DBContext *
make_new_db() noexcept
{
  return new DBContext();
}
void
free_db(DBContext *ptr) noexcept
{
  delete ptr;
}

void
advance_new_block(void *db) noexcept
{
  auto *ptr = reinterpret_cast<DBContext *>(db);
  if (!ptr)
    return;

  ptr->advance_new_block();
}

void *
open_runtime_context() noexcept
{
  auto *out = new std::shared_ptr<hogd::MockRuntimeContext>();
  *out = std::make_shared<hogd::MockRuntimeContext>(
      make_runtime_context(RuntimeEngine::kWasmi));
  return out;
}
void
close_runtime_context(void *context) noexcept
{
  delete static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);
}
bool
add_contract_and_method(void *context, const unsigned char *contract_hash,
                        ForeignEntryPoint entry_point,
                        void *foreign_context) noexcept
{
  if (contract_hash == nullptr)
    return false;

  hogd::SHA256Hash hash;
  std::memcpy(hash.data(), contract_hash, sizeof(hogd::SHA256Hash));

  if (context == nullptr)
    return false;
  if (entry_point == nullptr)
    return false;

  auto *c = static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);

  if (!(*c)) {
    return false;
  }

  (*c)->set_trampoline(hash, entry_point, foreign_context);
  return true;
}

void *
open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx_,
                     unsigned int serial_len) noexcept
{
  if (!db) {
    return nullptr;
  }
  auto *db_context = reinterpret_cast<hogd::DBContext *>(db);
  if (!rc) {
    return nullptr;
  }
  if (!db_context->is_block_open()) {
    return nullptr;
  }

  auto *runtime_context =
      reinterpret_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(rc);

  if (!signed_tx_) {
    return nullptr;
  }
  hogd::SignedTransaction stx;
  try {
    x23::deserialize(&stx,
                     std::span<const unsigned char>(signed_tx_, serial_len));
  } catch (x23::XdrBadInput const &e) {
    return nullptr;
  }

  return new SyscallHandlerContext(*runtime_context, stx, *db_context);
}

void
close_syscall_handler(void *syscall_handler) noexcept
{
  delete static_cast<SyscallHandlerContext *>(syscall_handler);
}

constexpr ForeignRootInvokeResult kUnrecoverableError = {
    .result = {.result_code = 0,
               .panic = std::to_underlying(InvokeErrorCode::kUnrecoverable)},
    .gas_consumed = 0};

ForeignRootInvokeResult
root_invoke(void *syscall_handler) noexcept
{
  if (!syscall_handler) {
    return kUnrecoverableError;
  }

  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);

  if (!context->push_context()) {
    return kUnrecoverableError;
  }

  auto res = context->root_invoke();

  context->pop_context();

  return res;
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

ForeignHostFnResult
from_status(HostFnStatus<SyscallErrorCode> status)
{
  if (status) {
    return recoverable_error(*status);
  }
  return ForeignHostFnResult{.result_code = 0,
                             .panic =
                                 std::to_underlying(status.error().code())};
}

template<typename Arg>
std::optional<Arg>
read_input_arg(std::span<const uint8_t> input)
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

ForeignHostFnResult
mock_sha256(std::span<const uint8_t> input, std::span<uint8_t> output)
{
  auto input_arg = read_input_arg<SHA256_input>(input);
  if (!input_arg) {
    return unrecoverable_error();
  }

  auto *handler = SyscallHandlerContext::get_context();
  if (handler == nullptr) {
    return unrecoverable_error();
  }

  auto output_arg = read_input_arg<SHA256_output>(output);
  if (!output_arg) {
    return unrecoverable_error();
  }

  // ignoring gas consumption
  auto [status, _] =
      syscall_SHA256_inner(std::as_bytes(std::span(input_arg->buffer)),
                           output_arg->hash);

  if (!write_output_arg(*output_arg, output)) {
    return unrecoverable_error();
  }

  return from_status(status);
}

ForeignHostFnResult
mock_syscall_entrypoint(int32_t syscall, const unsigned char *input,
                        uint32_t input_len, unsigned char *output,
                        uint32_t output_len) noexcept
{
  std::span<const uint8_t> input_span = std::span(input, input_len);
  std::span<uint8_t> output_span = std::span(output, output_len);

  switch (static_cast<SyscallNo>(syscall)) {
  case SyscallNo::kSHA256:
    return mock_sha256(input_span, output_span);
  default:
    return recoverable_error(SyscallErrorCode::kNoSyscall);
  }
}

} // namespace

} // namespace hogd

#ifdef __cplusplus
extern "C" {
#endif

void *
create_db()
{
  return hogd::make_new_db();
}
void
free_db(void *ptr)
{
  hogd::free_db(static_cast<hogd::DBContext *>(ptr));
}

void
advance_new_block(void *db)
{
  hogd::advance_new_block(db);
}

void *
open_runtime_context()
{
  return hogd::open_runtime_context();
}

void
close_runtime_context(void *context)
{
  hogd::close_runtime_context(context);
}
int
add_contract_and_method(void *context, const unsigned char *contract_hash,
                        ForeignEntryPoint entry_point, void *foreign_context)
{
  return (hogd::add_contract_and_method(context, contract_hash, entry_point,
                                        foreign_context))
             ? 0
             : 1;
}

void *
open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx_,
                     unsigned int serial_len)
{
  return hogd::open_syscall_handler(db, rc, signed_tx_, serial_len);
}

void
close_syscall_handler(void *syscall_handler)
{
  hogd::close_syscall_handler(syscall_handler);
}

struct ForeignRootInvokeResult
root_invoke(void *syscall_handler)
{
  return hogd::root_invoke(syscall_handler);
}

struct ForeignHostFnResult
mock_syscall_entrypoint(int32_t syscall, const unsigned char *input,
                        uint32_t input_len, unsigned char *output,
                        uint32_t output_len)
{
  return hogd::mock_syscall_entrypoint(syscall, input, input_len, output,
                                       output_len);
}

#ifdef __cplusplus
}
#endif
