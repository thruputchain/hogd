// -*-C++-*-

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/scheduler.h"
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

  x23::Async<x23::XdrVec<x23::Ptr<SignedTransaction>>>
  schedgateway_lookup_tx(TransactionHashBatch tx_hashes)
  {
    // This result shape has no in-band error arm, so reject an
    // oversized batch by throwing: the x23 dispatcher converts handler
    // exceptions into a SYSTEM_ERR reply, the same category it uses
    // for requests whose argument counts are too large to decode.
    if (tx_hashes.size() > kMaxGatewayBatchSize) {
      throw std::runtime_error(
          std::format("schedgateway_lookup_tx: batch size {} exceeds "
                      "maximum {}",
                      tx_hashes.size(), kMaxGatewayBatchSize));
    }
    co_return co_await coord_->schedcoord_lookup_transactions(tx_hashes);
  }

  x23::Async<PendingTxCount> schedgateway_pending_tx_count()
  {
    // size() is the mempool runqueue + stranded count — the full
    // "pending tx" view from the scheduler's perspective.  It can
    // transiently dip below zero while add_txs()/schedule() race (see
    // Scheduler::size_), so clamp before it becomes the wire count.
    co_return std::max<ssize_t>(0, coord_->scheduler()->size());
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
