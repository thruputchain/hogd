#include "hogd/test_ffi/test_bindings.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "coord.hh"
#include "ffi_enums.hh"
#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/mock_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/format.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {
const std::string db_to_exec_port = std::to_string(kDBToExecutorPortNum);
const std::string db_to_coord_port = std::to_string(kDBToCoordPortNum);
const std::string exec_to_coord_port = std::to_string(kExecToCoordPortNum);

class DBContext {
public:
  using db_client_t = DatabaseToCoordInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  DBContext()
    : db_(1, db_to_exec_port, db_to_coord_port), pool_(1), cp_(pool_),
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
      std::printf("invoke success\n");
      return ForeignRootInvokeResult{
          .result = ForeignInvokeResult{.result_code = *res.result_,
                                        .panic = std::to_underlying(
                                            InvokeErrorCode::kNone)},
          .gas_consumed = res.gas_consumed_};
    }

    std::printf("invoke failed\n");
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

  std::printf("got tx: %s\n", x23::show(stx).c_str());

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

#ifdef __cplusplus
}
#endif
