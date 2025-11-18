#pragma once

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/hogdb/rpc_context.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class HogDBAuthRpc {
public:
  using rpc_service = DBAuth1;
  explicit HogDBAuthRpc(RPCContext &ctx) : ctx_(ctx) {}

  x23::Async<DBAuthResult> authdb_get_auth_data(BlockID block_id,
                                                ContractAddress address);

  x23::Async<DBAuthResultBatch>
  authdb_get_auth_data_batch(BlockID block_id, ContractAddressBatch addresses);

  static x23::Async<void> dbauth_intro(AuthCard card)
  {
    // TODO(geoff): implement subscription to auth data changes
    co_return;
  }

private:
  RPCContext &ctx_;
};

static_assert(is_DBAuth1<HogDBAuthRpc *, x23::Async>);

class HogDBAuthServer : public RpcServer<HogDBAuthRpc> {
public:
  explicit HogDBAuthServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
