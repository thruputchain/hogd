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
#include "hogd/hogexec/common/block_id_util.h"
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

void
ViewGatewayRpc::shut_down()
{
  shutdown_flag_ = true;
  shutdown_flag_.notify_all();

  std::scoped_lock g(view_state_mutex_);
  for (auto &[block_id, arc] : view_states_) {
    arc.initiate_shutdown();
  }
  current_view_block_id_ = 0;
}

void
ViewGatewayRpc::gc_view_state(BlockID block_id)
{
  ctx_.remove_shutdown_block_tx(block_id);
  std::scoped_lock g(view_state_mutex_);
  view_states_.erase(block_id);
}

PerThreadARC<ViewGatewayRpc::ViewState> *
ViewGatewayRpc::get_current_view_state()
{
  std::scoped_lock g(view_state_mutex_);
  if (current_view_block_id_ == 0) {
    if (shutdown_flag_.load(std::memory_order_relaxed)) {
      return nullptr;
    }
    BlockID block_id = generate_block_id();
    auto status = ctx_.open_block_tx(
        block_id, BlockExecutionMode::kBlockExecutionModeViewing);
    if (!status.has_value()) {
      warn("viewgateway: failed to open viewing block tx");
      return nullptr;
    }
    view_states_.try_emplace(block_id, block_id, db_proxy_);
    current_view_block_id_ = block_id;
  }
  auto *state = &view_states_.at(current_view_block_id_);
  if (!state->try_acquire_reference()) {
    return nullptr;
  }
  return state;
}

bool
ViewGatewayRpc::rotate_view_state(BlockID stale_block_id)
{
  std::scoped_lock g(view_state_mutex_);
  if (current_view_block_id_ != stale_block_id) {
    return true;
  }
  view_states_.at(stale_block_id).initiate_shutdown();

  BlockID new_block_id = generate_block_id();
  auto status = ctx_.open_block_tx(
      new_block_id, BlockExecutionMode::kBlockExecutionModeViewing);
  if (!status.has_value()) {
    warn("viewgateway: failed to open replacement viewing block tx");
    current_view_block_id_ = 0;
    return false;
  }
  view_states_.try_emplace(new_block_id, new_block_id, db_proxy_);
  current_view_block_id_ = new_block_id;
  return true;
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

    auto *state = get_current_view_state();
    if (!state) {
      continue;
    }

    BlockID state_block_id = state->get().block_id;
    ViewGatewayResult result =
        run_view_attempt(query, tip, state->get());

    if (result.outcome() != ViewGatewayOutcome::ViewGatewayAbandoned) {
      if (state->release_reference()) {
        gc_view_state(state_block_id);
      }
      return result;
    }

    if (!rotate_view_state(state_block_id)) {
      if (state->release_reference()) {
        gc_view_state(state_block_id);
      }
      return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    if (state->release_reference()) {
      gc_view_state(state_block_id);
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
