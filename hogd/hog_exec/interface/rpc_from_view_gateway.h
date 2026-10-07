// -*-C++-*-

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

#include "hogd/base/perthread_arc.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "view.hh"
#include "view_gateway.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

// Executor-owned read-only invocation server exposed to the public RPC gateway.
// The response height is captured from the DbTip used to construct the
// execution snapshot for the successful attempt.
class ViewGatewayRpc {
public:
  using rpc_service = ViewGateway1;

  ViewGatewayRpc(std::string const &db_executor_address,
                 std::string const &event_host, std::uint16_t event_exec_port);

  static ViewGatewayIntroOutcome
  viewgateway_intro(ProtocolCommit const &protocol);

  x23::Async<ViewGatewayResult> viewgateway_view(TransactionViewQuery query);
  // The budget includes queue time and is checked between calls and retries.
  // It does not interrupt a running VM or a pending database RPC. The public
  // gateway must also enforce its response deadline.
  x23::Async<ViewGatewayBatchResult>
  viewgateway_view_batch(ViewGatewayBatchQuery query);

  void shut_down();

  // Signal that the current ViewState should be replaced with a fresh
  // snapshot before the next view attempt.  This is a no-op if there is
  // no current ViewState.  Thread-safe; used by tests and by the background
  // refresh worker when it observes a newer committed tip.
  void request_refresh()
  {
    refresh_flag_.store(true, std::memory_order_release);
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  friend class ViewGatewayBatchTest;
  using ViewDeadline = std::chrono::steady_clock::time_point;
  static constexpr std::uint32_t kMaxViewAttempts = 3;

  // A view stands in for a hypothetical block on top of the tip: height is
  // the tip's plus one, and since no block has been proposed there is no
  // agreed timepoint, so use now. TimePoint is Unix nanoseconds.
  static BlockExecutionContext view_block_execution_context(DbTip const &tip)
  {
    return BlockExecutionContext{
        .block_height = tip.value.proposal_header.block_height + 1,
        .block_timepoint = static_cast<TimePoint>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()),
    };
  }

  struct ViewState {
    BlockID block_id;
    DbTip tip;
    StorageSnapshotCache snapshot_cache;

    // tip is declared first, so read it rather than the moved-from t.
    ViewState(BlockID id, DbTip t, DatabaseProxy &db_proxy)
      : block_id(id), tip(std::move(t)),
        snapshot_cache(id, view_block_execution_context(tip), db_proxy)
    {
      // The gateway fetched this tip when it opened the viewing snapshot;
      // seed it rather than paying load_db_tip()'s second fetch.
      snapshot_cache.set_db_tip(tip);
    }
  };

  x23::Async<ViewGatewayResult>
  run_view_with_retries(TransactionViewQuery const &query);
  x23::Async<ViewGatewayResult>
  run_view_attempt(TransactionViewQuery const &query, DbTip const &tip,
                   ViewState &view_state);

  x23::Async<ViewGatewayBatchResult>
  run_batch_with_retries(ViewGatewayBatchQuery const &query,
                         ViewDeadline deadline);
  x23::Async<ViewGatewayBatchResult>
  run_batch_attempt(ViewGatewayBatchQuery const &query, ViewState &state,
                    ViewDeadline deadline);

  // Returns a pointer to the current view state with a reference acquired,
  // or nullptr if shutdown is in progress. Lazy-initializes the first
  // viewing block tx. Caller must eventually call release_reference().
  x23::Async<PerThreadARC<ViewState> *> get_current_view_state();

  // Rotates the current view state after an Abandon. Initiates shutdown on
  // the old state and opens a new viewing block tx. Returns false if the
  // new block tx could not be opened.
  x23::Async<bool> rotate_view_state(BlockID stale_block_id);

  x23::Async<void> gc_view_state(BlockID block_id);

  void refresh_worker(std::stop_token const &stoken);
  x23::Async<void> probe_refresh_tip();
  x23::Async<void> close_all_view_states();
  x23::Async<std::optional<std::pair<BlockID, BlockHeight>>> current_view_pin();

  std::atomic<bool> shutdown_flag_ = false;
  std::atomic<bool> refresh_flag_ = false;
  DatabaseProxy db_proxy_;
  EventServerProxy event_server_proxy_;

  // A CoroLock, not a std::mutex: it is held across HogDb reads that now
  // suspend, and the view path runs on x23 ThreadPool workers where blocking
  // for a mutex would park a worker (and where a coroutine can resume on a
  // thread that did not lock it). Every holder releases it with
  // `co_await g.reset_and_return(...)` rather than letting ~CoroGuard run,
  // which would resume the next waiter as a nested call and can build an
  // unbounded unlock->resume chain under contention.
  x23::CoroLock view_state_lock_;
  BlockID current_view_block_id_ = 0;
  std::unordered_map<BlockID, PerThreadARC<ViewState>> view_states_;
  std::jthread refresh_thread_;
};

static_assert(is_ViewGateway1<ViewGatewayRpc *, x23::MaybeAsync>);

class ViewGatewayServer : public RpcServer<ViewGatewayRpc> {
public:
  ViewGatewayServer(std::string const &db_executor_address,
                    std::string const &event_host,
                    std::uint16_t event_exec_port)
    : RpcServer(db_executor_address, event_host, event_exec_port)
  {}
};

ViewGatewayResult
view_gateway_result_from_raw_view_result(BlockHeight height,
                                         ExpectedViewResult raw);

TransactionViewQuery normalize_public_view_query(TransactionViewQuery query);

} // namespace hogd
