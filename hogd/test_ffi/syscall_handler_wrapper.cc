#include "hogd/test_ffi/syscall_handler_wrapper.h"

#include <utility>

#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

SyscallHandlerContext::SyscallHandlerContext(MockRuntimeContext *context,
                                             SignedTransaction const &stx,
                                             DBContext &db)
  : identifier_(db.allocate_identifier()), db_(db),
    handler_(stx.tx, db.get_snapshot(), db.get_new_db_proxy(identifier_),
             db.get_new_event_server_proxy(identifier_), ProposalModeCtor{},
             *context),
     is_mocked_handler_(false)

{}

SyscallHandlerContext::SyscallHandlerContext(MockRuntimeContext *context,
                                             DBContext &db,
                                             AuthClaimData const &auth_claim_data,
                                             SHA256Hash const &source_tx_hash)
: identifier_(db.allocate_identifier()), db_(db),
  handler_(db.get_snapshot(), db.get_new_db_proxy(identifier_),
  db.get_new_event_server_proxy(identifier_), ProposalModeCtor{},
  *context, auth_claim_data, source_tx_hash),
  is_mocked_handler_(true)
{}

ForeignRootInvokeResult
SyscallHandlerContext::root_invoke()
{
  constexpr auto kTestHarnessFailure = ForeignRootInvokeResult{.result = ForeignInvokeResult{.panic = std::to_underlying(
                                    InvokeErrorCode::kInvokeUnrecoverable)},
                                   .gas_consumed = 0};
  if (is_mocked_handler_) {
    return kTestHarnessFailure;
  }
  if (!push_context()) {
    return kTestHarnessFailure;
  }
  x23::Defer _([this]() { pop_context(); });

  auto res = handler_.root_invoke();

  if (res.result_) {
    return ForeignRootInvokeResult{.result =
                                       ForeignInvokeResult{
                                           .panic = std::to_underlying(
                                               InvokeErrorCode::kInvokeNone)},
                                   .gas_consumed = res.gas_consumed_};
  }

  return ForeignRootInvokeResult{.result = {.panic = std::to_underlying(
                                                res.result_.error().code())},
                                 .gas_consumed = res.gas_consumed_};
}

} // namespace hogd
