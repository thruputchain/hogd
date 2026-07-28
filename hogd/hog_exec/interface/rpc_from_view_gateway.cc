#include "hogd/hog_exec/interface/rpc_from_view_gateway.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/db_shard_routing/sharding.h" // kShardFunctionId
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/view/view.h"
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

constexpr auto kRefreshProbeInterval = std::chrono::milliseconds(100);

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
          .shard_id = 0,
          .total_shards = 1,
          .shard_function_id = kShardFunctionId,
          .address_for_exec = db_executor_address.c_str(),
          .address_for_sched = "",
          .address_for_auth = "",
      }},
      .event = {},
  });
}

} // namespace

ViewGatewayRpc::ViewGatewayRpc(std::string const &db_executor_address,
                               std::string const &event_host,
                               std::uint16_t event_exec_port,
                               std::size_t nthreads)
  : db_proxy_(make_view_database_identifier(db_executor_address)),
    event_server_proxy_(x23::HostPort{event_host, event_exec_port}),
    worker_pool_(static_cast<int>(nthreads))
{
  refresh_thread_ = std::jthread(
      [this](std::stop_token const &stoken) { refresh_worker(stoken); });
}

ViewGatewayIntroOutcome
ViewGatewayRpc::viewgateway_intro(ProtocolCommit const &protocol)
{
  if (std::string_view(protocol) !=
      std::string_view(version::kHogdProtocol())) {
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
  refresh_thread_.request_stop();

  std::scoped_lock g(view_state_mutex_);
  for (auto it = view_states_.begin(); it != view_states_.end();) {
    if (it->second.initiate_shutdown()) {
      auto close_res = db_proxy_.close_viewing_snapshot(it->first);
      if (!close_res.has_value()) {
        warn("viewgateway: failed to close viewing snapshot {} on shutdown",
             it->first);
      }
      it = view_states_.erase(it);
    }
    else {
      ++it;
    }
  }
  current_view_block_id_ = 0;
}

void
ViewGatewayRpc::gc_view_state(BlockID block_id)
{
  auto close_res = db_proxy_.close_viewing_snapshot(block_id);
  if (!close_res.has_value()) {
    warn("viewgateway: failed to close viewing snapshot {}", block_id);
  }
  std::scoped_lock g(view_state_mutex_);
  view_states_.erase(block_id);
}

void
ViewGatewayRpc::refresh_worker(std::stop_token const &stoken)
{
  std::mutex cv_mutex;
  std::condition_variable_any cv;

  while (!stoken.stop_requested()) {
    std::unique_lock g(cv_mutex);
    cv.wait_for(g, stoken, kRefreshProbeInterval, [] { return false; });

    if (stoken.stop_requested() ||
        shutdown_flag_.load(std::memory_order_relaxed)) {
      break;
    }
    probe_refresh_tip();
  }
}

void
ViewGatewayRpc::probe_refresh_tip()
{
  BlockID block_id = 0;
  BlockHeight view_height = 0;
  {
    std::scoped_lock g(view_state_mutex_);
    if (current_view_block_id_ == 0 ||
        shutdown_flag_.load(std::memory_order_relaxed)) {
      return;
    }
    ViewState const &current_state =
        view_states_.at(current_view_block_id_).get();
    block_id = current_state.block_id;
    view_height = current_state.tip.value.proposal_header.block_height;
  }

  auto tip_res = db_proxy_.get_db_tip(block_id);
  bool should_refresh =
      !tip_res.has_value() ||
      tip_res.value().value.proposal_header.block_height > view_height;
  if (!should_refresh) {
    return;
  }

  std::scoped_lock g(view_state_mutex_);
  if (current_view_block_id_ != block_id ||
      shutdown_flag_.load(std::memory_order_relaxed)) {
    return;
  }
  ViewState const &current_state = view_states_.at(block_id).get();
  if (!tip_res.has_value() ||
      tip_res.value().value.proposal_header.block_height >
          current_state.tip.value.proposal_header.block_height) {
    refresh_flag_.store(true, std::memory_order_release);
  }
}

PerThreadARC<ViewGatewayRpc::ViewState> *
ViewGatewayRpc::get_current_view_state()
{
  std::scoped_lock g(view_state_mutex_);

  if (current_view_block_id_ != 0 &&
      refresh_flag_.exchange(false, std::memory_order_acquire)) {
    // The background refresh worker probes HogDb at a bounded cadence and sets
    // refresh_flag_ when the cached viewing snapshot falls behind the committed
    // tip.  The hot view path only consumes that flag so repeated views can
    // keep reusing the snapshot without polling HogDb on every request.
    auto res = db_proxy_.open_viewing_snapshot();
    if (res.has_value() &&
        res.value().viewing_block_id != current_view_block_id_) {
      BlockID old_block_id = current_view_block_id_;
      bool can_gc = view_states_.at(old_block_id).initiate_shutdown();

      auto &snapshot = res.value();
      view_states_.try_emplace(snapshot.viewing_block_id,
                               snapshot.viewing_block_id, snapshot.tip,
                               db_proxy_);
      current_view_block_id_ = snapshot.viewing_block_id;

      if (can_gc) {
        auto close_res = db_proxy_.close_viewing_snapshot(old_block_id);
        if (!close_res.has_value()) {
          warn("viewgateway: failed to close refreshed-out viewing snapshot {}",
               old_block_id);
        }
        view_states_.erase(old_block_id);
      }
    }
    else if (!res.has_value()) {
      warn("viewgateway: failed to open refreshed viewing snapshot");
    }
  }

  if (current_view_block_id_ == 0) {
    if (shutdown_flag_.load(std::memory_order_relaxed)) {
      return nullptr;
    }
    auto res = db_proxy_.open_viewing_snapshot();
    if (!res.has_value()) {
      warn("viewgateway: failed to open viewing snapshot");
      return nullptr;
    }
    auto &snapshot = res.value();
    view_states_.try_emplace(snapshot.viewing_block_id,
                             snapshot.viewing_block_id, snapshot.tip,
                             db_proxy_);
    current_view_block_id_ = snapshot.viewing_block_id;
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

  auto res = db_proxy_.open_viewing_snapshot();
  if (!res.has_value()) {
    warn("viewgateway: failed to open replacement viewing snapshot");
    current_view_block_id_ = 0;
    return false;
  }
  auto &snapshot = res.value();
  view_states_.try_emplace(snapshot.viewing_block_id, snapshot.viewing_block_id,
                           snapshot.tip, db_proxy_);
  current_view_block_id_ = snapshot.viewing_block_id;
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

    auto *state = get_current_view_state();
    if (!state) {
      return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    BlockID state_block_id = state->get().block_id;
    ViewGatewayResult result =
        run_view_attempt(query, state->get().tip, state->get());

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
                                 DbTip const &tip, ViewState &view_state)
{
  static thread_local auto context = make_hog_exec_context();

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
  return view_gateway_result_from_raw_view_result(
      tip.value.proposal_header.block_height, std::move(raw));
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

} // namespace hogd
