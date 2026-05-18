// -*-C++-*-

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "hogd/base/perthread_arc.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogexec/interface/dispatch_pool.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "view.hh"
#include "view_gateway.hh"
#include "x23/async.h"

namespace hogd {

// Executor-owned read-only invocation server exposed to the public RPC gateway.
// The response height is captured from the DbTip used to construct the
// execution snapshot for the successful attempt.
class ViewGatewayRpc {
public:
  using rpc_service = ViewGateway1;

  ViewGatewayRpc(RPCContext &ctx, std::string const &db_executor_address,
                 std::string const &event_host, std::uint16_t event_exec_port,
                 std::size_t nthreads = 2);

  static ViewGatewayIntroOutcome viewgateway_intro(Protocol const &protocol);

  x23::Async<ViewGatewayResult> viewgateway_view(TransactionViewQuery query);

  struct ViewState {
    BlockID block_id;
    StorageSnapshotCache snapshot_cache;

    ViewState(BlockID id, DatabaseProxy &db_proxy)
      : block_id(id), snapshot_cache(id, db_proxy)
    {}
  };

  void shut_down()
  {
    shutdown_flag_ = true;
    shutdown_flag_.notify_all();

    std::scoped_lock g(view_state_mutex_);
    if (view_state_) {
      BlockID block_id = view_state_->get().block_id;
      if (view_state_->initiate_shutdown()) {
        ctx_.remove_shutdown_block_tx(block_id);
      }
      (void)view_state_.release();
    }
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  static constexpr std::uint32_t kMaxViewAttempts = 3;

  ViewGatewayResult run_view_with_retries(TransactionViewQuery const &query);
  ViewGatewayResult run_view_attempt(TransactionViewQuery const &query,
                                     DbTip const &tip,
                                     ViewState &view_state);

  void gc_view_state(PerThreadARC<ViewState> *state)
  {
    ctx_.remove_shutdown_block_tx(state->get().block_id);
    delete state;
  }

  std::atomic<bool> shutdown_flag_ = false;
  RPCContext &ctx_;
  DatabaseProxy db_proxy_;
  EventServerProxy event_server_proxy_;
  DispatchPool worker_pool_;

  std::mutex view_state_mutex_;
  std::unique_ptr<PerThreadARC<ViewState>> view_state_;
};

static_assert(is_ViewGateway1<ViewGatewayRpc *, x23::MaybeAsync>);

class ViewGatewayServer : public RpcServer<ViewGatewayRpc> {
public:
  ViewGatewayServer(RPCContext &ctx, std::string const &db_executor_address,
                    std::string const &event_host,
                    std::uint16_t event_exec_port)
    : RpcServer(ctx, db_executor_address, event_host, event_exec_port)
  {}
};

ViewGatewayResult
view_gateway_result_from_raw_view_result(BlockHeight height,
                                         ExpectedViewResult raw);

TransactionViewQuery normalize_public_view_query(TransactionViewQuery query);

ViewGatewayResult run_view_with_retries_for_gateway(
    TransactionViewQuery const &query, std::atomic<bool> const &shutdown_flag,
    std::function<DbTip()> const &read_tip,
    std::function<ViewGatewayResult(TransactionViewQuery const &,
                                    DbTip const &)> const &run_attempt,
    std::uint32_t max_attempts);

} // namespace hogd
