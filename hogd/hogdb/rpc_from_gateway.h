// -*-C++-*-

#pragma once

#include <atomic>
#include <utility>
#include <vector>

// generated x23 headers
#include "db_gateway.hh"
#include "network.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/rpc_context.h"

namespace hogd {

// Read-only x23 RPC server exposed from Db (hogdb) to the public RPC
// gateway (see protocol/db_gateway.x for the XDR).  Serves
// committed-snapshot reads.
//
// The only method is dbgateway_get_items, a batch read.  Every
// returned item is consistent with the same committed block height,
// which is included in the response.
class HogDBGatewayRpc {
public:
  using rpc_service = DbGateway1;

  // One shard: the whole batch is served from it, which is what a hogdb process
  // holding a single shard wants.
  explicit HogDBGatewayRpc(RPCContext &ctx) : shards_{&ctx} {}

  // Every shard, in shard-id order (see order_shards_by_id), for a process that
  // holds them all: each key is served by the shard shard_of_key picks, so the
  // batch is split and reassembled into input order. The manifest advertises
  // one dbGateway endpoint, so the split has to happen behind this RPC rather
  // than by the caller asking each shard.
  explicit HogDBGatewayRpc(std::vector<RPCContext *> shards)
    : shards_(std::move(shards))
  {
    CHECK(!shards_.empty());
  }

  static DbGatewayIntroOutcome dbgateway_intro(ProtocolCommit const &protocol);

  DbGatewayResult dbgateway_get_items(AddressAndKeyBatch const &batch);

  void shut_down() { shutdown_flag_ = true; }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
  std::vector<RPCContext *> shards_;
};

static_assert(is_DbGateway1<HogDBGatewayRpc *, std::type_identity_t>);

class HogDBGatewayServer : public RpcServer<HogDBGatewayRpc> {
public:
  explicit HogDBGatewayServer(RPCContext &ctx) : RpcServer(ctx) {}
  explicit HogDBGatewayServer(std::vector<RPCContext *> shards)
    : RpcServer(std::move(shards))
  {}
};

} // namespace hogd
