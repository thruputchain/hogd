#pragma once

#include <utility>

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

  x23::Async<DBAuthResult> authdb_get_auth_data(BlockHeight height,
                                                ContractAddress address);

  x23::Async<DBAuthResultBatch>
  authdb_get_auth_data_batch(BlockHeight height,
                             ContractAddressBatch addresses);

  x23::Async<bool> dbauth_intro(AuthCard card)
  {
    return ctx_.get_auth_change_subscription_manager()
        .add_full_auth_change_subscriber(std::move(card));
  }

  BlockHeight get_expected_query_height();

private:
  RPCContext &ctx_;
};

static_assert(is_DBAuth1<HogDBAuthRpc *, x23::Async>);

class HogDBAuthServer : public RpcServer<HogDBAuthRpc> {
public:
  explicit HogDBAuthServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
