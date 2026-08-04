#include "hogd/auth/dbauth_client.h"

#include <cstddef>
#include <optional>
#include <vector>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/status.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/db_shard_routing/sharding.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

namespace {

// A free coroutine, not a lambda: the intro fan-out below holds these Asyncs
// across suspension, and a lambda's captures die with the closure object.
x23::Async<BlockHeight>
intro_one_shard(dbauth_client_t &client, AuthCard const &card)
{
  DBAuthIntroResult res = co_await client.dbauth_intro(card);
  CHECK_EQ(res.outcome(), DBAuthIntroOutcome::kIntroSuccess);
  co_return res.current_height();
}

} // namespace

x23::Async<AuthStatus<std::optional<AddressAuthData>>>
DBAuthClient::lookup_auth_data(BlockHeight height, ContractAddress address)
{
  // Must repeat the routing the write used, or an existing account reads back
  // as kDBAuthNotFound.
  size_t const shard = shard_of_address(address, clients_.shard_count());
  auto res =
      co_await clients_.shard(shard).authdb_get_auth_data(height, address);

  switch (res.error()) {
  case DBAuthError::kDBAuthSuccess:
    co_return AuthStatus<std::optional<AddressAuthData>>{res.auth_data()};
  case DBAuthError::kDBAuthNotFound:
    co_return AuthStatus<std::optional<AddressAuthData>>{std::nullopt};
  case DBAuthError::kDBAuthStaleBlockHeight:
    co_return AuthError(AuthErrorCode::kAuthStaleBlockHeight);
  case DBAuthError::kDBAuthInternal:
    co_return AuthError(AuthErrorCode::kAuthInternal);
  }
  error("unknown DBAuthError: {}", static_cast<int>(res.error()));
  co_return AuthError(AuthErrorCode::kAuthInternal);
}

x23::Async<BlockHeight>
DBAuthClient::subscribe_to_auth_data_changes(AuthCard const &card)
{
  std::vector<BlockHeight> heights =
      co_await clients_.broadcast_collect<BlockHeight>(
          [&card](dbauth_client_t &client) {
            return intro_one_shard(client, card);
          });

  // Auth keys one cache off one height, so a divergent set has no height to
  // initialize to -- fail-stop rather than serve reads against a stale shard.
  BlockHeight const height = heights[kDefaultShard];
  for (size_t shard = 0; shard < heights.size(); ++shard) {
    if (heights[shard] != height) {
      panic("db shard {} reports height {} but shard {} reports {}; shard tips "
            "must agree",
            shard, heights[shard], kDefaultShard, height);
    }
  }
  co_return height;
}

} // namespace hogd
