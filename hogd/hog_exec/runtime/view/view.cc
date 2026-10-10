#include "hogd/hog_exec/runtime/view/view.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/auth_subtoken.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/validated_auth_tokens.h"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

x23::Async<ExpectedViewResult>
View::run_view_transaction(StorageSnapshotCache &snapshot_cache,
                           PerTransactionDatabaseProxy proxy,
                           PerTransactionEventServerProxy event_proxy,
                           TransactionViewQuery const &query,
                           RuntimeContext &context,
                           NondeterministicProposalData ndet)
{
  ViewingModeCtor ctor{.proposal_data_segment_ = std::move(ndet)};
  SystemCallHandler syscall_handler(snapshot_cache, std::move(proxy),
                                    std::move(event_proxy), std::move(ctor),
                                    context, query.auth_claim, SHA256Hash{});

  assert(!syscall_handler.is_running());

  if (!query.forwarded_tokens.empty() && !query.parent_address) {
    ExpectedViewResult result(TransactionViewStatus::InvalidInput);
    result.error_code() =
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput;
    co_return result;
  }

  if (query.parent_address) {
    Invocation parent_frame_invocation = Invocation{
        .invoked_address = *query.parent_address,
        .entrypoint = entrypoint_from_method_id(0),
        .gas_limit = UINT64_MAX,
        .calldata = Calldata{},
    };

    auto r = syscall_handler.start_frame(parent_frame_invocation, 0, {});
    if (!r.has_value()) {
      co_return ExpectedViewResult{TransactionViewStatus::InternalError};
    }
    // the stashed parent trace is only non-null if tracing is on
    CHECK(!r.value());
  }

  // Forwarded tokens arrive serialized; decode them under the chain's depth
  // limit, as MintAuthToken decodes the tokens it mints.
  std::vector<RegisteredAuthToken> forwarded_tokens;
  try {
    for (auto const &bytes : query.forwarded_tokens) {
      x23::deserialize(
          &forwarded_tokens.emplace_back(),
          x23::BufView{bytes.data(), bytes.size()},
          size_t{syscall_handler.config_params().max_auth_token_depth});
    }
  } catch (x23::XdrException const &) {
    ExpectedViewResult result(TransactionViewStatus::InvalidInput);
    result.error_code() =
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput;
    co_return result;
  } catch (...) {
    error("unrecoverable: failed to decode forwarded auth tokens");
    co_return ExpectedViewResult{TransactionViewStatus::InternalError};
  }

  // No expiry_height validation here: a token that expired before the current
  // block can still legitimately appear in a frame's forwarded inputs.
  std::vector<uint32_t> forwarded_token_ids;
  forwarded_token_ids.reserve(forwarded_tokens.size());

  for (auto &token : forwarded_tokens) {
    auto token_id = syscall_handler.mint_auth_token(std::move(token));
    if (!token_id.has_value()) {
      co_return ExpectedViewResult{TransactionViewStatus::InternalError};
    }
    forwarded_token_ids.push_back(token_id.value());
  }

  // Freshly minted ids never repeat, so this only fails on a broken registry.
  auto forwarded_auth_tokens =
      ValidatedAuthTokens::parse(std::as_bytes(std::span(forwarded_token_ids)));
  if (!forwarded_auth_tokens.has_value()) {
    error("unrecoverable: view forwarded-token ids repeat");
    co_return ExpectedViewResult{TransactionViewStatus::InternalError};
  }

  // Admission checks invoke() expects its caller to have run: the frame-level
  // checks first, then that the caller owns the tokens it forwards.
  auto admitted =
      syscall_handler.validate_new_invocation(query.invocation.invoked_address,
                                              query.invoke_flags);
  if (admitted.has_value()) {
    admitted =
        syscall_handler.validate_forwarded_auth_tokens(*forwarded_auth_tokens);
  }
  if (!admitted.has_value()) {
    info("view transaction failed with SyscallErrorCode::{}",
         admitted.error().code());
    ExpectedViewResult result(TransactionViewStatus::InvalidInput);
    result.error_code() = admitted.error().code();
    co_return result;
  }

  auto [res, gas_consumed] = co_await syscall_handler.invoke(
      query.invocation.invoked_address,
      sdk_method_id(query.invocation.entrypoint), query.invocation.gas_limit,
      query.invocation.calldata, query.invoke_flags,
      std::move(*forwarded_auth_tokens));

  if (!res.has_value()) {
    switch (res.error().code()) {
    case SystemErrorCode::ValidationFailure:
      error("view transaction failed with SystemErrorCode::ValidationFailure");
      co_return ExpectedViewResult(
          TransactionViewStatus::NondeterministicDataValidationFailure);
    case SystemErrorCode::Abandon:
      co_return ExpectedViewResult{TransactionViewStatus::Abandoned};
    case SystemErrorCode::Unrecoverable:
      error("view transaction failed with SystemErrorCode::Unrecoverable");
      co_return ExpectedViewResult(TransactionViewStatus::InternalError);
    }
  }

  // A SyscallError means the invoke was rejected before a child frame ran; a
  // frame that ran carries its outcome (Return / Abort / Fault) via the frame
  // output below.
  SyscallStatus<InvocationFrameStatusCode> const &frame = *res;
  if (!frame.has_value()) {
    info("view transaction failed with SyscallErrorCode::{}",
         frame.error().code());
    ExpectedViewResult result(TransactionViewStatus::InvalidInput);
    result.error_code() = frame.error().code();
    co_return result;
  }

  assert(syscall_handler.last_frame_finished() == (!query.parent_address));

  ExpectedViewResult result(TransactionViewStatus::Ok);
  // TODO(geoff): we could almost certainly elide a copy or two here
  result.result().output = syscall_handler.get_most_recent_frame_output();
  result.result().gas_consumed = gas_consumed;
  result.result().events = syscall_handler.get_most_recent_frame_events();
  co_return result;
}

} // namespace hogd
