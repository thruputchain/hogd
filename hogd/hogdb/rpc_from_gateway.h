// -*-C++-*-

#pragma once

#include <atomic>

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

  explicit HogDBGatewayRpc(RPCContext &ctx) : ctx_(ctx) {}

  static DbGatewayIntroOutcome dbgateway_intro(ProtocolCommit const &protocol);

  DbGatewayResult dbgateway_get_items(AddressAndKeyBatch const &batch);

  void shut_down() { shutdown_flag_ = true; }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
  RPCContext &ctx_;
};

static_assert(is_DbGateway1<HogDBGatewayRpc *, std::type_identity_t>);

class HogDBGatewayServer : public RpcServer<HogDBGatewayRpc> {
public:
  explicit HogDBGatewayServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
