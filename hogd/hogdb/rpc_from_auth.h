#pragma once

#include <string_view>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/version.h"
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

  x23::Async<DBAuthIntroResult> dbauth_intro(AuthCard card)
  {
    if (std::string_view(card.protocol) != std::string_view(version::kHogdProtocol())) {
      co_return DBAuthIntroResult(DBAuthIntroOutcome::kIntroFailure);
    }
    if (!co_await ctx_.get_auth_change_subscription_manager()
             .add_full_auth_change_subscriber(std::move(card))) {
      co_return DBAuthIntroResult(DBAuthIntroOutcome::kIntroFailure);
    }
    DBAuthIntroResult ret(DBAuthIntroOutcome::kIntroSuccess);
    ret.current_height() = get_expected_query_height();
    co_return ret;
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
