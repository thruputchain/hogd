#include "hogd/hogexec/runtime/view/view.h"

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"

namespace hogd {

ExpectedViewResult
View::run_view_transaction(StorageSnapshotCache &snapshot_cache,
                           PerTransactionDatabaseProxy proxy,
                           PerTransactionEventServerProxy event_proxy,
                           TransactionViewQuery const &query,
                           RuntimeContext &context)
{
  SystemCallHandler syscall_handler(snapshot_cache, proxy,
                                    std::move(event_proxy), ViewingModeCtor{},
                                    context, query.auth_claim, SHA256Hash{});

  assert(!syscall_handler.is_running());

  std::vector<uint32_t> forwarded_token_ids;
  forwarded_token_ids.reserve(query.forwarded_tokens.size());

  for (auto const &token : query.forwarded_tokens) {
    auto token_id = syscall_handler.mint_auth_token(token);
    if (!token_id.has_value()) {
      return ExpectedViewResult{TransactionViewStatus::InternalError};
    }
    forwarded_token_ids.push_back(token_id.value());
  }

  if (query.parent_address) {
    Invocation parent_frame_invocation = Invocation{
        .invoked_address = *query.parent_address,
        .entrypoint = entrypoint_from_method_id(0),
        .gas_limit = UINT64_MAX,
        .calldata = Calldata{},
    };

    auto r = syscall_handler.start_frame(parent_frame_invocation, 0, {});
    // r is only non-null if tracing is on
    CHECK(!r);
  }

  auto [res, gas_consumed] =
      syscall_handler.invoke(query.invocation.invoked_address,
                             sdk_method_id(query.invocation.entrypoint),
                             query.invocation.gas_limit,
                             query.invocation.calldata, query.invoke_flags,
                             forwarded_token_ids);

  if (!res.has_value()) {
    switch (res.error().code()) {
    case SystemErrorCode::ValidationFailure:
      error("view transaction failed with SystemErrorCode::ValidationFailure");
      return ExpectedViewResult(TransactionViewStatus::InternalError);
    case SystemErrorCode::Abandon:
      return ExpectedViewResult{TransactionViewStatus::Abandoned};
    case SystemErrorCode::Unrecoverable:
      error("view transaction failed with SystemErrorCode::Unrecoverable");
      return ExpectedViewResult(TransactionViewStatus::InternalError);
    }
  }

  if (*res != SyscallErrorCode::kSyscallErrorCodeNone &&
      *res != SyscallErrorCode::kSyscallErrorCodeFrameReverted &&
      *res != SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
    info("view transaction failed with SystemErrorCode::{}", *res);
    ExpectedViewResult result(TransactionViewStatus::InvalidInput);
    result.error_code() = *res;
    return result;
  }

  assert(syscall_handler.last_frame_finished() == (!query.parent_address));

  ExpectedViewResult result(TransactionViewStatus::Ok);
  // TODO(geoff): we could almost certainly elide a copy or two here
  result.result().output = syscall_handler.get_most_recent_frame_output();
  result.result().events = syscall_handler.get_most_recent_frame_events();
  return result;
}

} // namespace hogd
