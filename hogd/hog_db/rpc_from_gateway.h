// -*-C++-*-

#pragma once

#include <atomic>
#include <cstddef>

// generated x23 headers
#include "db_gateway.hh"
#include "network.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/rpc_context.h"

namespace hogd {

// Read-only x23 RPC server exposed from Db (hog_db) to the public RPC
// gateway (see protocol/db_gateway.x for the XDR).  Serves
// committed-snapshot reads.
//
// The only method is dbgateway_get_items, a batch read.  Every
// returned item is consistent with the same committed block height,
// which is included in the response.
class HogDbGatewayRpc {
public:
  using rpc_service = DbGateway1;

  explicit HogDbGatewayRpc(RPCContext &ctx) : ctx_(ctx) {}

  // Bounds on a single dbgateway_get_items batch (an unbounded XDR array);
  // over-limit batches are rejected in dbgateway_get_items. kMaxBatchItems
  // caps the read count and the fixed-size result vector (one slot per
  // item, found or not). kMaxResponseBytes caps the cumulative serialized
  // size, since items are variable-sized (a bytestring up to
  // kBytestringCeilingLen, an AuthData row up to kMaxSigners), keeping the
  // retained content and wire response within the RPC message budget.
  static constexpr std::size_t kMaxBatchItems = 16384;
  static constexpr std::size_t kMaxResponseBytes = 15UL * 1024 * 1024;

  static DbGatewayIntroOutcome dbgateway_intro(ProtocolCommit const &protocol);

  // A coroutine: it may suspend to wait out a commit publication, and the
  // rpc server runs handlers on ThreadPool workers, which must not park.
  x23::Async<DbGatewayResult>
  dbgateway_get_items(AddressAndKeyBatch const &batch);

  void shut_down() { shutdown_flag_ = true; }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
  RPCContext &ctx_;
};

// MaybeAsync rather than type_identity_t: dbgateway_get_items is a coroutine
// (it can suspend on a commit publication), which is also the transform
// RpcServer itself validates.
static_assert(is_DbGateway1<HogDbGatewayRpc *, x23::MaybeAsync>);

class HogDbGatewayServer : public RpcServer<HogDbGatewayRpc> {
public:
  explicit HogDbGatewayServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
