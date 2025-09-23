#include "hogd/test_ffi/test_bindings.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include "account.hh"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/mock_syscall_entry.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

void
lazy_init()
{
  static std::mutex init_mutex;
  std::lock_guard lock(init_mutex);
  static bool initialized = false;
  if (!initialized) {
    hogd_init(0, nullptr);
    initialized = true;
  }
}

DBContext *
make_new_db() noexcept
{
  lazy_init();
  try {
    return new DBContext();
  } catch (std::exception const &e) {
    error("error creating db context: {}", e.what());
    return nullptr;
  }
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
  lazy_init();
  std::shared_ptr<hogd::MockRuntimeContext> *out;
  try {
    out = new std::shared_ptr<hogd::MockRuntimeContext>();
  } catch (std::exception const &e) {
    error("error creating runtime context: {}", e.what());
    return nullptr;
  }
  *out = std::make_shared<hogd::MockRuntimeContext>(make_hogexec_context());
  return out;
}

void
close_runtime_context(void *context) noexcept
{
  delete static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);
}

bool
register_new_contract(void *context, int32_t mock_contract_id,
                      ForeignEntryPoint entry_point,
                      void *foreign_context) noexcept
{
  if (context == nullptr)
    return false;
  if (entry_point == nullptr)
    return false;

  auto *c = static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);

  if (!(*c)) {
    return false;
  }

  (*c)->set_trampoline(mock_contract_id, entry_point, foreign_context);
  return true;
}

void *
open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx,
                     unsigned int signed_tx_len) noexcept
{
  if (!db) {
    return nullptr;
  }
  auto *db_context = reinterpret_cast<hogd::DBContext *>(db);
  if (!rc) {
    return nullptr;
  }

  auto *runtime_context =
      reinterpret_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(rc);

  if (!signed_tx) {
    return nullptr;
  }
  try {
    hogd::SignedTransaction stx;
    x23::deserialize(&stx, x23::BufView{signed_tx, signed_tx_len});
    auto *out = static_cast<void *>(
        new SyscallHandlerContext(*runtime_context, stx, *db_context));
    return out;
  } catch (std::exception const &e) {
    try {
      error("error creating syscall handler context: {}", e.what());
    } catch (...) {
      return nullptr;
    }
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

void
close_syscall_handler(void *syscall_handler) noexcept
{
  try {
    delete static_cast<SyscallHandlerContext *>(syscall_handler);
  } catch (std::exception const &e) {
    try {
      error("error closing syscall handler: {}", e.what());
    } catch (...) {
      return;
    }
  } catch (...) {
    return;
  }
}

constexpr ForeignRootInvokeResult kUnrecoverableError = {
    .result = {.panic =
                   std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)},
    .gas_consumed = 0};

ForeignRootInvokeResult
root_invoke(void *syscall_handler) noexcept
{
  try {
    if (!syscall_handler) {
      error("root_invoke error: no syscall handler!");
      return kUnrecoverableError;
    }

    auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
    auto res = context->root_invoke();
    return res;
  } catch (std::exception const &e) {
    try {
      error("error during root_invoke: {}", e.what());
    } catch (...) {
      return kUnrecoverableError;
    }
  } catch (...) {
    return kUnrecoverableError;
  }
  return kUnrecoverableError;
}

constexpr StorageCostModel kCostModel(12000, 1, 5, 100, 5);

void
setup_grant_contract_storage_tokens(void *db, const uint8_t *contract_addr,
                                    int64_t tokens)
{
  if (db == nullptr) {
    return;
  }
  auto *db_cast = static_cast<DBContext *>(db);
  hogd::ContractAddress addr;
  std::memcpy(addr.data(), contract_addr, addr.size());

  db_cast->get_base_db().write_item(
      AddressAndKey{
          .address = addr,
          .key =
              StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance),
      },
      std::make_shared<StorageItemWrapper>(int128_t{tokens}, &kCostModel));
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
register_new_contract(void *context, int32_t mock_contract_id,
                      ForeignEntryPoint entry_point, void *foreign_context)
{
  return (hogd::register_new_contract(context, mock_contract_id, entry_point,
                                      foreign_context))
             ? 0
             : 1;
}

int
setup_create_contract(void *context, void *db, uint8_t const *contract_bytes,
                      uint32_t contract_len)
{
  if (context == nullptr || db == nullptr) {
    return 1;
  }
  auto *context_cast =
      static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);
  auto *db_cast = static_cast<hogd::DBContext *>(db);

  hogd::Contract contract;
  try {
    x23::deserialize(&contract, x23::BufView{contract_bytes, contract_len});
  } catch (x23::XdrException &e) {
    return 1;
  }

  // make sure the contract has already been registered with the instantiation
  // context
  if (contract.contract_type() == hogd::ContractType::kMockTest) {
    if (!((*context_cast)->has_mock_contract(contract.mock_contract_id()))) {
      return 1;
    }
  }

  auto h = sha256(contract);

  db_cast->get_base_db().write_contract(h, std::make_shared<hogd::Contract>(
                                               std::move(contract)));
  return 0;
}

int
setup_deploy_contract(void *db, const uint8_t *deployment_addr,
                      const uint8_t *contract_hash, uint32_t contract_size)
{
  if (db == nullptr) {
    return 1;
  }
  auto *db_cast = static_cast<hogd::DBContext *>(db);

  hogd::SHA256Hash h;
  std::memcpy(h.data(), contract_hash, h.size());
  hogd::ContractAddress addr;
  std::memcpy(addr.data(), deployment_addr, addr.size());
  std::shared_ptr<hogd::StorageItemWrapper> item =
      std::make_shared<hogd::StorageItemWrapper>(
          hogd::DeployedContractHash{.hash = h, .contract_size = contract_size},
          &hogd::kCostModel);

  db_cast->get_base_db().write_item(
      hogd::AddressAndKey{
          .address = addr,
          .key = hogd::StorageKey(
              hogd::StorageKeyType::kStorageKeyTypeAddressMetadata)},
      std::move(item));
  return 0;
}

void *
open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx,
                     unsigned int signed_tx_len)
{
  return hogd::open_syscall_handler(db, rc, signed_tx, signed_tx_len);
}

void
setup_grant_contract_storage_tokens(void *db, const uint8_t *contract_addr,
                                    int64_t tokens)
{
  hogd::setup_grant_contract_storage_tokens(db, contract_addr, tokens);
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
  hogd::swap_logger(
      std::make_unique<hogd::ConsoleLogger>(hogd::LogLevel::kTrace));
}

#ifdef __cplusplus
}
#endif
