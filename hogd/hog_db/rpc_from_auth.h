#pragma once

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/version.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class HogDbAuthRpc {
public:
  using rpc_service = DBAuth1;
  explicit HogDbAuthRpc(RPCContext &ctx) : ctx_(ctx) {}

  x23::Async<DBAuthResult> authdb_get_auth_data(BlockHeight height,
                                                ContractAddress address);

  x23::Async<DBAuthResultBatch>
  authdb_get_auth_data_batch(BlockHeight height,
                             ContractAddressBatch addresses);

  x23::Async<DBAuthIntroResult> dbauth_intro(AuthCard card)
  {
    if (std::string_view(card.protocol) !=
        std::string_view(version::kHogdProtocol())) {
      co_return DBAuthIntroResult(DBAuthIntroOutcome::kIntroFailure);
    }
    std::optional<BlockHeight> last_published =
        co_await ctx_.get_auth_change_subscription_manager()
            .add_full_auth_change_subscriber(std::move(card));
    if (!last_published) {
      co_return DBAuthIntroResult(DBAuthIntroOutcome::kIntroFailure);
    }
    DBAuthIntroResult ret(DBAuthIntroOutcome::kIntroSuccess);
    // Report at least the highest height already published to other
    // subscribers when this one was inserted. If the new subscriber missed that
    // height's publish (it was dispatched before the insert), reporting the
    // bare committed height would leave its auth server parked on the missing
    // height forever (issue #2814). Covering it makes the missed height stale.
    ret.current_height() =
        std::max(get_expected_query_height(), *last_published);
    co_return ret;
  }

  BlockHeight get_expected_query_height();

private:
  RPCContext &ctx_;
};

static_assert(is_DBAuth1<HogDbAuthRpc *, x23::Async>);

class HogDbAuthServer : public RpcServer<HogDbAuthRpc> {
public:
  explicit HogDbAuthServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
