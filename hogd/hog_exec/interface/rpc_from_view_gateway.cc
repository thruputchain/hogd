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
    event_server_proxy_(x23::HostPort{event_host, event_exec_port})
{
  // nthreads no longer sizes a view-side pool: view execution is coroutinized
  // and runs on the x23 ThreadPool. Kept in the signature so callers' plumbing
  // stays put while that pool is retuned.
  (void)nthreads;
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
  co_return co_await run_view_with_retries(query);
}

void
ViewGatewayRpc::shut_down()
{
  shutdown_flag_ = true;
  shutdown_flag_.notify_all();
  refresh_thread_.request_stop();

  // Called from the node's control path on its own thread, never an x23
  // ThreadPool worker, so it blocks on the coroutine that does the work -- and
  // it must be a coroutine, because taking view_state_lock_ requires awaiting.
  close_all_view_states()();
  current_view_block_id_ = 0;
}

x23::Async<void>
ViewGatewayRpc::close_all_view_states()
{
  x23::CoroGuard g;
  co_await g(view_state_lock_);
  for (auto it = view_states_.begin(); it != view_states_.end();) {
    if (it->second.initiate_shutdown()) {
      auto close_res = co_await db_proxy_.close_viewing_snapshot(it->first);
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
  co_await g.reset_and_return();
  panic("unreachable");
}

x23::Async<void>
ViewGatewayRpc::gc_view_state(BlockID block_id)
{
  auto close_res = co_await db_proxy_.close_viewing_snapshot(block_id);
  if (!close_res.has_value()) {
    warn("viewgateway: failed to close viewing snapshot {}", block_id);
  }
  x23::CoroGuard g;
  co_await g(view_state_lock_);
  view_states_.erase(block_id);
  co_await g.reset_and_return();
  panic("unreachable");
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
    // The refresh loop owns this thread and is never an x23 ThreadPool
    // worker, so it blocks on the probe rather than awaiting it.
    probe_refresh_tip()();
  }
}

// Reads the block id and height the view path is currently pinned to, or
// nullopt when there is nothing pinned (or we are shutting down). Split out so
// this guarded region can leave through reset_and_return -- see the note on
// view_state_lock_.
x23::Async<std::optional<std::pair<BlockID, BlockHeight>>>
ViewGatewayRpc::current_view_pin()
{
  using result_t = std::optional<std::pair<BlockID, BlockHeight>>;
  x23::CoroGuard g;
  co_await g(view_state_lock_);
  if (current_view_block_id_ == 0 ||
      shutdown_flag_.load(std::memory_order_relaxed)) {
    co_await g.reset_and_return(result_t{std::nullopt});
    panic("unreachable");
  }
  ViewState const &current_state =
      view_states_.at(current_view_block_id_).get();
  co_await g.reset_and_return(result_t{
      std::pair{current_state.block_id,
                current_state.tip.value.proposal_header.block_height}});
  panic("unreachable");
}

x23::Async<void>
ViewGatewayRpc::probe_refresh_tip()
{
  auto pin = co_await current_view_pin();
  if (!pin.has_value()) {
    co_return;
  }
  auto [block_id, view_height] = *pin;

  auto tip_res = co_await db_proxy_.get_db_tip(block_id);
  bool should_refresh =
      !tip_res.has_value() ||
      tip_res.value().value.proposal_header.block_height > view_height;
  if (!should_refresh) {
    co_return;
  }

  x23::CoroGuard g;
  co_await g(view_state_lock_);
  if (current_view_block_id_ != block_id ||
      shutdown_flag_.load(std::memory_order_relaxed)) {
    co_await g.reset_and_return();
    panic("unreachable");
  }
  ViewState const &current_state = view_states_.at(block_id).get();
  if (!tip_res.has_value() ||
      tip_res.value().value.proposal_header.block_height >
          current_state.tip.value.proposal_header.block_height) {
    refresh_flag_.store(true, std::memory_order_release);
  }
  co_await g.reset_and_return();
  panic("unreachable");
}

// Returns the pinned view state with a reference acquired, or nullptr.
//
// Holds view_state_lock_ across the HogDb snapshot open/close below, which is
// exactly why that lock is a CoroLock; every exit goes through
// reset_and_return.
x23::Async<PerThreadARC<ViewGatewayRpc::ViewState> *>
ViewGatewayRpc::get_current_view_state()
{
  using result_t = PerThreadARC<ViewState> *;
  x23::CoroGuard g;
  co_await g(view_state_lock_);

  if (current_view_block_id_ != 0 &&
      refresh_flag_.exchange(false, std::memory_order_acquire)) {
    // The background refresh worker probes HogDb at a bounded cadence and sets
    // refresh_flag_ when the cached viewing snapshot falls behind the committed
    // tip.  The hot view path only consumes that flag so repeated views can
    // keep reusing the snapshot without polling HogDb on every request.
    auto res = co_await db_proxy_.open_viewing_snapshot();
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
        auto close_res =
            co_await db_proxy_.close_viewing_snapshot(old_block_id);
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
      co_await g.reset_and_return(result_t{nullptr});
      panic("unreachable");
    }
    auto res = co_await db_proxy_.open_viewing_snapshot();
    if (!res.has_value()) {
      warn("viewgateway: failed to open viewing snapshot");
      co_await g.reset_and_return(result_t{nullptr});
      panic("unreachable");
    }
    auto &snapshot = res.value();
    view_states_.try_emplace(snapshot.viewing_block_id,
                             snapshot.viewing_block_id, snapshot.tip,
                             db_proxy_);
    current_view_block_id_ = snapshot.viewing_block_id;
  }
  auto *state = &view_states_.at(current_view_block_id_);
  if (!state->try_acquire_reference()) {
    co_await g.reset_and_return(result_t{nullptr});
    panic("unreachable");
  }
  co_await g.reset_and_return(result_t{state});
  panic("unreachable");
}

x23::Async<bool>
ViewGatewayRpc::rotate_view_state(BlockID stale_block_id)
{
  x23::CoroGuard g;
  co_await g(view_state_lock_);
  if (current_view_block_id_ != stale_block_id) {
    co_await g.reset_and_return(true);
    panic("unreachable");
  }
  view_states_.at(stale_block_id).initiate_shutdown();

  auto res = co_await db_proxy_.open_viewing_snapshot();
  if (!res.has_value()) {
    warn("viewgateway: failed to open replacement viewing snapshot");
    current_view_block_id_ = 0;
    co_await g.reset_and_return(false);
    panic("unreachable");
  }
  auto &snapshot = res.value();
  view_states_.try_emplace(snapshot.viewing_block_id, snapshot.viewing_block_id,
                           snapshot.tip, db_proxy_);
  current_view_block_id_ = snapshot.viewing_block_id;
  co_await g.reset_and_return(true);
  panic("unreachable");
}

x23::Async<ViewGatewayResult>
ViewGatewayRpc::run_view_with_retries(TransactionViewQuery const &query)
{
  for (std::uint32_t attempt = 0; attempt < kMaxViewAttempts; ++attempt) {
    if (shutdown_flag_.load(std::memory_order_relaxed)) {
      warn("viewgateway: shutdown requested before view attempt");
      co_return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    auto *state = co_await get_current_view_state();
    if (!state) {
      co_return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    BlockID state_block_id = state->get().block_id;
    ViewGatewayResult result =
        co_await run_view_attempt(query, state->get().tip, state->get());

    if (result.outcome() != ViewGatewayOutcome::ViewGatewayAbandoned) {
      if (state->release_reference()) {
        co_await gc_view_state(state_block_id);
      }
      co_return result;
    }

    if (!co_await rotate_view_state(state_block_id)) {
      if (state->release_reference()) {
        co_await gc_view_state(state_block_id);
      }
      co_return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }

    if (state->release_reference()) {
      co_await gc_view_state(state_block_id);
    }

    if (shutdown_flag_.load(std::memory_order_relaxed)) {
      warn("viewgateway: shutdown requested after abandoned view attempt");
      co_return make_view_gateway_result(
          ViewGatewayOutcome::ViewGatewayInternalError);
    }
  }

  co_return make_view_gateway_result(ViewGatewayOutcome::ViewGatewayAbandoned);
}

x23::Async<ViewGatewayResult>
ViewGatewayRpc::run_view_attempt(TransactionViewQuery const &query,
                                 DbTip const &tip, ViewState &view_state)
{
  static thread_local auto context = make_hog_exec_context();

  // WAI: empty nondeterministic data causes ViewingModeData to default
  // TryInvoke outcomes to success. This is deliberate — see the comment on
  // ViewingModeData::view_try_invoke_bit() in execution_mode.h.
  ExpectedViewResult raw = co_await View::run_view_transaction(
      view_state.snapshot_cache,
      PerTransactionDatabaseProxy(db_proxy_, view_state.block_id,
                                  std::numeric_limits<TransactionID>::max()),
      PerTransactionEventServerProxy(event_server_proxy_, view_state.block_id,
                                     std::numeric_limits<TransactionID>::max()),
      query, *context, {});
  co_return view_gateway_result_from_raw_view_result(
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
