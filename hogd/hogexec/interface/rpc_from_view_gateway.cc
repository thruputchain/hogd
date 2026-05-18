#include "hogd/hogexec/interface/rpc_from_view_gateway.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/view/view.h"
#include "hogd/version.h"
#include "invoke_flag.hh"
#include "network.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"
#include "view_gateway.hh"
#include "x23/async.h"

namespace hogd {

namespace {

ViewGatewayResult
make_view_gateway_result(ViewGatewayOutcome outcome)
{
  return ViewGatewayResult(outcome);
}

AuthClaim
make_public_view_auth_claim()
{
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 0;
  claim.source_address_claim().required_weight = 0;
  return claim;
}

DatabaseIdentifier
make_view_database_identifier(std::string const &db_executor_address)
{
  return ShardedDB(IntroExecResult{
      .exec_id = 0,
      .shards = {ShardCard{
          .protocol = version::kHogdProtocol(),
          .upid = {},
          .block_height = 0,
          .address_for_exec = db_executor_address.c_str(),
          .address_for_sched = "",
          .address_for_auth = "",
      }},
      .event = {},
  });
}

} // namespace

ViewGatewayRpc::ViewGatewayRpc(RPCContext &ctx,
                               std::string const &db_executor_address,
                               std::string const &event_host,
                               std::uint16_t event_exec_port,
                               std::size_t nthreads)
  : ctx_(ctx), db_proxy_(make_view_database_identifier(db_executor_address)),
    event_server_proxy_(make_bind_host_port(event_host, event_exec_port)),
    worker_pool_(static_cast<int>(nthreads))
{}

ViewGatewayIntroOutcome
ViewGatewayRpc::viewgateway_intro(Protocol const &protocol)
{
  if (strcmp(protocol.c_str(), version::kHogdProtocol()) != 0) {
    return ViewGatewayIntroOutcome::ViewGatewayIntroMismatchedProtocol;
  }
  return ViewGatewayIntroOutcome::ViewGatewayIntroSuccess;
}

x23::Async<ViewGatewayResult>
ViewGatewayRpc::viewgateway_view(TransactionViewQuery query)
{
  query = normalize_public_view_query(std::move(query));
  x23::Awaitable<ViewGatewayResult> awaitable;
  x23::CoroCB<ViewGatewayResult> cb(awaitable);
  worker_pool_.submit([this, query = std::move(query), cb]() {
    cb(run_view_with_retries(query));
  });
  co_return co_await awaitable;
}

ViewGatewayResult
ViewGatewayRpc::run_view_with_retries(TransactionViewQuery const &query)
{
  for (std::uint32_t attempt = 0; attempt < kMaxViewAttempts; ++attempt) {
    if (shutdown_flag_.load(std::memory_order_relaxed)) {
      warn("viewgateway: shutdown requested before view attempt");
      return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    DbTip tip = ctx_.get_tip();
    if (!RPCContext::has_committed_tip(tip)) {
      warn("viewgateway: no committed DbTip is available");
      return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    // Acquire a reference on the current view state, lazy-initializing on the
    // first request by opening a viewing block tx.
    PerThreadARC<ViewState> *state_ptr = nullptr;
    {
      std::scoped_lock g(view_state_mutex_);
      if (!view_state_) {
        BlockID block_id = generate_block_id();
        auto status = ctx_.open_block_tx(
            block_id, BlockExecutionMode::kBlockExecutionModeViewing);
        if (!status.has_value()) {
          warn("viewgateway: failed to open viewing block tx");
          return make_view_gateway_result(
              ViewGatewayOutcome::ViewGatewayInternalError);
        }
        view_state_ =
            std::make_unique<PerThreadARC<ViewState>>(block_id, db_proxy_);
      }
      state_ptr = view_state_.get();
    }

    if (!state_ptr->try_acquire_reference()) {
      // Shutdown in progress on this arc — retry and we'll get the new one.
      continue;
    }

    ViewGatewayResult result =
        run_view_attempt(query, tip, state_ptr->get());

    if (result.outcome() != ViewGatewayOutcome::ViewGatewayAbandoned) {
      if (state_ptr->release_reference()) {
        gc_view_state(state_ptr);
      }
      return result;
    }

    // Abandoned: rotate to a fresh viewing block tx.
    {
      std::scoped_lock g(view_state_mutex_);
      // Check if someone already rotated while we waited for the mutex.
      if (view_state_.get() == state_ptr) {
        view_state_->initiate_shutdown();
        // Release from unique_ptr without destroying — the PerThreadARC
        // destructor asserts shutdown_pending_refcount_ == 0, which only
        // holds after all refs drain. The last release_reference() caller
        // will gc via gc_view_state().
        view_state_.release();

        BlockID new_block_id = generate_block_id();
        auto status = ctx_.open_block_tx(
            new_block_id, BlockExecutionMode::kBlockExecutionModeViewing);
        if (!status.has_value()) {
          warn("viewgateway: failed to open replacement viewing block tx");
          // Release our ref on the old state.
          if (state_ptr->release_reference()) {
            gc_view_state(state_ptr);
          }
          return make_view_gateway_result(
              ViewGatewayOutcome::ViewGatewayInternalError);
        }
        view_state_ = std::make_unique<PerThreadARC<ViewState>>(
            new_block_id, db_proxy_);
      }
    }

    // Release our ref on the old (now-shutting-down) state.
    if (state_ptr->release_reference()) {
      gc_view_state(state_ptr);
    }

    if (shutdown_flag_.load(std::memory_order_relaxed)) {
      warn("viewgateway: shutdown requested after abandoned view attempt");
      return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }
  }

  return make_view_gateway_result(ViewGatewayOutcome::ViewGatewayAbandoned);
}

ViewGatewayResult
ViewGatewayRpc::run_view_attempt(TransactionViewQuery const &query,
                                 DbTip const &tip,
                                 ViewState &view_state)
{
  static thread_local auto context = make_hogexec_context();

  // WAI: empty nondeterministic data causes ViewingModeData to default
  // TryInvoke outcomes to success. This is deliberate — see the comment on
  // ViewingModeData::view_try_invoke_bit() in execution_mode.h.
  ExpectedViewResult raw = View::run_view_transaction(
      view_state.snapshot_cache,
      PerTransactionDatabaseProxy(db_proxy_, view_state.block_id,
                                  std::numeric_limits<TransactionID>::max()),
      PerTransactionEventServerProxy(event_server_proxy_, view_state.block_id,
                                     std::numeric_limits<TransactionID>::max()),
      query, *context, {});
  return view_gateway_result_from_raw_view_result(tip.height, std::move(raw));
}

ViewGatewayResult
view_gateway_result_from_raw_view_result(BlockHeight height,
                                         ExpectedViewResult raw)
{
  // TODO(view-gateway): add a caller-error ViewGatewayOutcome. InvalidInput
  // and bad user-supplied ndet are request problems, but v1 can only express
  // Ok, Abandoned, or InternalError.
  switch (raw.status()) {
  case TransactionViewStatus::Ok:
  {
    ViewGatewayResult result(ViewGatewayOutcome::ViewGatewayOk);
    result.payload().height = height;
    result.payload().output = std::move(raw.result().output);
    result.payload().events = std::move(raw.result().events);
    return result;
  }
  case TransactionViewStatus::InvalidInput:
  {
    warn("viewgateway: collapsing invalid view input to Fault: {}",
         std::to_underlying(raw.error_code()));
    ViewGatewayResult result(ViewGatewayOutcome::ViewGatewayOk);
    result.payload().height = height;
    result.payload().output =
        InvocationFrameOutput(InvocationFrameStatusCode::Fault);
    result.payload().events = {};
    return result;
  }
  case TransactionViewStatus::Abandoned:
    return make_view_gateway_result(ViewGatewayOutcome::ViewGatewayAbandoned);
  case TransactionViewStatus::InternalError:
  case TransactionViewStatus::NondeterministicDataValidationFailure:
    return make_view_gateway_result(
        ViewGatewayOutcome::ViewGatewayInternalError);
  }

  return make_view_gateway_result(ViewGatewayOutcome::ViewGatewayInternalError);
}

// Normalize a caller-supplied view query for unauthenticated public use.
// We strip auth_claim (no real signer) and invoke_flags (no TryInvoke /
// DelegateCall), but intentionally preserve parent_address and
// forwarded_tokens.  View queries are read-only (no state commits), so
// letting the caller choose a parent_address — which the invoked contract
// sees via GetMsgSender — is safe and gives off-chain tooling the ability
// to simulate calls from any context.
TransactionViewQuery
normalize_public_view_query(TransactionViewQuery query)
{
  query.auth_claim = make_public_view_auth_claim();
  query.invoke_flags = std::to_underlying(InvokeFlag::None);
  return query;
}

ViewGatewayResult
run_view_with_retries_for_gateway(
    TransactionViewQuery const &query, std::atomic<bool> const &shutdown_flag,
    std::function<DbTip()> const &read_tip,
    std::function<ViewGatewayResult(TransactionViewQuery const &,
                                    DbTip const &)> const &run_attempt,
    std::uint32_t max_attempts)
{
  for (std::uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
    if (shutdown_flag.load(std::memory_order_relaxed)) {
      warn("viewgateway: shutdown requested before view attempt");
      return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    DbTip tip = read_tip();
    if (!RPCContext::has_committed_tip(tip)) {
      warn("viewgateway: no committed DbTip is available");
      return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    ViewGatewayResult result = run_attempt(query, tip);
    if (result.outcome() != ViewGatewayOutcome::ViewGatewayAbandoned) {
      return result;
    }

    if (shutdown_flag.load(std::memory_order_relaxed)) {
      warn("viewgateway: shutdown requested after abandoned view attempt");
      return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }
  }

  return make_view_gateway_result(ViewGatewayOutcome::ViewGatewayAbandoned);
}

} // namespace hogd
