#include "hogd/test_ffi/test_bindings.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <print>
#include <span>
#include <unistd.h>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/mock_syscall_entry.h"
#include "hogd/test_ffi/port_allocator.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

DBContext *
make_new_db() noexcept
{
  return new DBContext(PortAllocator::get());
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
  std::shared_ptr<hogd::MockRuntimeContext> *out;
  try {
    out = new std::shared_ptr<hogd::MockRuntimeContext>();
  } catch (std::exception const &e) {
    std::println("error creating runtime context: {}", e.what());
    return nullptr;
  }
  *out = std::make_shared<hogd::MockRuntimeContext>(
      make_hogexec_context(RuntimeEngine::kWasmi));
  return out;
}

void
close_runtime_context(void *context) noexcept
{
  delete static_cast<std::shared_ptr<hogd::MockRuntimeContext> *>(context);
}

bool
register_new_contract(void *context, uint32_t mock_contract_id,
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
  hogd::SignedTransaction stx;
  try {
    x23::deserialize(&stx,
                     std::span<const unsigned char>(signed_tx, signed_tx_len));
  } catch (x23::XdrBadInput const &e) {
    return nullptr;
  }
  try {
    auto *out = static_cast<void *>(
        new SyscallHandlerContext(*runtime_context, stx, *db_context));
    std::println("created syscall handler context {}", out);
    return out;
  } catch (std::exception const &e) {
    std::println("error creating syscall handler context: {}", e.what());
    return nullptr;
  }
}

void
close_syscall_handler(void *syscall_handler) noexcept
{
  std::println("closing syscall handler {}", syscall_handler);
  delete static_cast<SyscallHandlerContext *>(syscall_handler);
}

constexpr ForeignRootInvokeResult kUnrecoverableError = {
    .result = {.result_code = 0,
               .panic =
                   std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)},
    .gas_consumed = 0};

ForeignRootInvokeResult
root_invoke(void *syscall_handler) noexcept
{
  if (!syscall_handler) {
    std::println("no syscall handler!");
    return kUnrecoverableError;
  }

  auto *context = static_cast<SyscallHandlerContext *>(syscall_handler);
  auto res = context->root_invoke();
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
register_new_contract(void *context, uint32_t mock_contract_id,
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
    x23::deserialize(&contract, std::span(contract_bytes, contract_len));
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
                      const uint8_t *contract_hash)
{
  if (db == nullptr) {
    return 1;
  }
  auto *db_cast = static_cast<hogd::DBContext *>(db);

  hogd::SHA256Hash h;
  std::memcpy(h.data(), contract_hash, h.size());
  hogd::ContractAddress addr;
  std::memcpy(addr.data(), deployment_addr, addr.size());

  db_cast->get_base_db().write_deployment(std::move(addr), std::move(h));
  return 0;
}

void *
open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx,
                     unsigned int signed_tx_len)
{
  return hogd::open_syscall_handler(db, rc, signed_tx, signed_tx_len);
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
                        uint64_t arg6)
{
  return hogd::mock_syscall_entrypoint(syscall, arg1, arg2, arg3, arg4, arg5,
                                       arg6);
}

void
enable_syscall_logging()
{
  hogd::syscall_logfd = 1;
}

#ifdef __cplusplus
}
#endif
