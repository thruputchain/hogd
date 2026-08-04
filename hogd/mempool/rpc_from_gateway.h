// -*-C++-*-

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/version.h"
#include "network.hh"
#include "parallel.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_gateway.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

// SchedGateway1 server: the scheduler-side methods exposed to the
// public RPC gateway.  Delegates to MempoolCoordinatorRpc for the
// underlying mempool/scheduler operations.
class MempoolGatewayRpc {
public:
  using rpc_service = SchedGateway1;

  explicit MempoolGatewayRpc(MempoolCoordinatorRpc *coord) : coord_(coord) {}

  MempoolGatewayRpc(const MempoolGatewayRpc &) = delete;
  MempoolGatewayRpc &operator=(const MempoolGatewayRpc &) = delete;
  MempoolGatewayRpc(MempoolGatewayRpc &&) = delete;
  MempoolGatewayRpc &operator=(MempoolGatewayRpc &&) = delete;

  static x23::Async<SchedGatewayIntroOutcome>
  schedgateway_intro(ProtocolCommit protocol)
  {
    if (std::string_view(protocol) !=
        std::string_view(version::kHogdProtocol())) {
      co_return SchedGatewayIntroOutcome::SchedGatewayIntroMismatchedProtocol;
    }
    co_return SchedGatewayIntroOutcome::SchedGatewayIntroSuccess;
  }

  x23::Async<void>
  schedgateway_submit_tx_batch(ParallelSignedTransactionBatch batch)
  {
    co_await coord_->schedulercoord_submit_unvetted(std::move(batch));
    co_return;
  }

  x23::Async<ExpectedDBTip> schedgateway_get_tip()
  {
    SchedulerGetTipResult tip = co_await coord_->schedcoord_get_tip();
    if (tip.outcome() == SchedulerGetTipOutcome::SchedulerGetTipOK) {
      ExpectedDBTip out;
      out.error(RPCError::kRPCNone);
      out.db_tip() = tip.db_tip();
      co_return out;
    }
    // Pre-genesis or any other non-OK outcome: surface as kRPCNotFound.
    co_return ExpectedDBTip(RPCError::kRPCNotFound);
  }

  // Upper bound on the total serialized size of the transactions returned from
  // a single public-gateway lookup.  A lookup request carries one 32-byte hash
  // per transaction, but a matched transaction can be far larger, so without a
  // cap an unauthenticated caller could force an unbounded response -- and an
  // unbounded number of heap-backed transaction copies -- from a small request,
  // e.g. by repeating a single live transaction's hash many times.  A
  // legitimate wallet only looks up a handful of its own in-flight transactions
  // and never approaches this bound.  The trusted coordinator RPC
  // ([MempoolCoordinatorRpc::schedcoord_lookup_transactions]) is not subject to
  // this cap.
  static constexpr size_t kMaxLookupResponseBytes = 16UZ * 1024 * 1024;

  x23::Async<x23::XdrVec<x23::Ptr<SignedTransaction>>>
  schedgateway_lookup_tx(SignedTransactionHashBatch tx_hashes)
  {
    co_return coord_->lookup_transactions(tx_hashes, kMaxLookupResponseBytes);
  }

  x23::Async<PendingTxCount> schedgateway_pending_tx_count()
  {
    // size() is the mempool runqueue + stranded count — the full
    // "pending tx" view from the scheduler's perspective.
    co_return coord_->scheduler()->size();
  }

  void shut_down() { shutdown_flag_ = true; }
  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
  MempoolCoordinatorRpc *coord_;
};

static_assert(is_SchedGateway1<MempoolGatewayRpc *, x23::Async>);

class MempoolGatewayServer : public RpcServer<MempoolGatewayRpc> {
public:
  explicit MempoolGatewayServer(MempoolCoordinatorRpc *coord) : RpcServer(coord)
  {}
};

} // namespace hogd
