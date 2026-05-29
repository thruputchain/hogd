#include "hogd/hogexec/runtime/view/view.h"

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "auth_token.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/auth_token_registry.h"
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
                           RuntimeContext &context,
                           NondeterministicProposalData ndet)
{
  ViewingModeCtor ctor{.proposal_data_segment_ = std::move(ndet)};
  SystemCallHandler syscall_handler(snapshot_cache, proxy,
                                    std::move(event_proxy), std::move(ctor),
                                    context, query.auth_claim, SHA256Hash{});

  assert(!syscall_handler.is_running());

  if (!query.forwarded_tokens.empty() && !query.parent_address) {
    ExpectedViewResult result(TransactionViewStatus::InvalidInput);
    result.error_code() =
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput;
    return result;
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

  try {
    for (auto const &token : query.forwarded_tokens) {
      if (!AuthTokenRegistry::node_within_depth(token.root_node,
                                                kAuthTokenMaxDepth)) {
        ExpectedViewResult result(TransactionViewStatus::InvalidInput);
        result.error_code() =
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput;
        return result;
      }
    }
  } catch (...) {
    error("unrecoverable: failed to validate forwarded auth token depth");
    return ExpectedViewResult{TransactionViewStatus::InternalError};
  }

  // No expiry_height validation here: a token that expired before the current
  // block can still legitimately appear in a frame's forwarded inputs.
  std::vector<uint32_t> forwarded_token_ids;
  forwarded_token_ids.reserve(query.forwarded_tokens.size());

  for (auto const &token : query.forwarded_tokens) {
    auto token_id = syscall_handler.mint_auth_token(token);
    if (!token_id.has_value()) {
      return ExpectedViewResult{TransactionViewStatus::InternalError};
    }
    forwarded_token_ids.push_back(token_id.value());
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
      return ExpectedViewResult(
          TransactionViewStatus::NondeterministicDataValidationFailure);
    case SystemErrorCode::Abandon:
      return ExpectedViewResult{TransactionViewStatus::Abandoned};
    case SystemErrorCode::Unrecoverable:
      error("view transaction failed with SystemErrorCode::Unrecoverable");
      return ExpectedViewResult(TransactionViewStatus::InternalError);
    }
  }

  // A nonzero error code means the invoke was rejected before a child frame
  // ran; the frame outcome (Return / Abort / Fault) is carried by the frame
  // output below.
  if (res->code != SyscallErrorCode::kSyscallErrorCodeNone) {
    info("view transaction failed with SyscallErrorCode::{}", res->code);
    ExpectedViewResult result(TransactionViewStatus::InvalidInput);
    result.error_code() = res->code;
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
