#include "hogd/test_ffi/test_bindings.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <span>
#include <unistd.h>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/mock_syscall_entry.h"
#include "hogd/test_ffi/port_allocator.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "transaction.hh"
#include "x23/serialize.h"

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
  auto *out = new std::shared_ptr<hogd::MockRuntimeContext>();
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
add_contract_and_method(void *context, uint32_t mock_contract_id,
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

  return new SyscallHandlerContext(*runtime_context, stx, *db_context);
}

void
close_syscall_handler(void *syscall_handler) noexcept
{
  std::println("closing syscall handler{}", syscall_handler);
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
add_contract_and_method(void *context, uint32_t mock_contract_id,
                        ForeignEntryPoint entry_point, void *foreign_context)
{
  return (hogd::add_contract_and_method(context, mock_contract_id, entry_point,
                                        foreign_context))
             ? 0
             : 1;
}

void *
open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx,
                     unsigned int signed_tx_len)
{
  std::println("opening syscall handler");
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

#ifdef __cplusplus
}
#endif
