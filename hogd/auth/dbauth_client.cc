#include "hogd/auth/dbauth_client.h"

#include <optional>
#include <stdexcept>
#include <utility>

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/auth/status.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

auto
make_dbauth_client(IntroAuthResult intro_auth_result)
{
  CHECK(intro_auth_result.shards.size() == 1);
  x23::HostPort hostport;
  if (!hostport.parse(intro_auth_result.shards[0].address_for_auth)) {
    throw std::runtime_error("Failed to parse database shard address");
  }
  return make_rpc_client<DBAuth1>(hostport);
}

} // namespace

DBAuthClient::DBAuthClient(IntroAuthResult intro_auth_result)
  : client_(make_dbauth_client(std::move(intro_auth_result)))
{}

x23::Async<AuthStatus<std::optional<AccountAuthData>>>
DBAuthClient::lookup_auth_data(BlockHeight height, ContractAddress address)
{
  auto res = co_await client_.authdb_get_auth_data(height, address);

  switch (res.error()) {
  case DBAuthError::kDBAuthSuccess:
    co_return AuthStatus<std::optional<AccountAuthData>>{res.auth_data()};
  case DBAuthError::kDBAuthNotFound:
    co_return AuthStatus<std::optional<AccountAuthData>>{std::nullopt};
  case DBAuthError::kDBAuthStaleBlockHeight:
    co_return AuthError(AuthErrorCode::kAuthStaleBlockHeight);
  case DBAuthError::kDBAuthInternal:
    co_return AuthError(AuthErrorCode::kAuthInternal);
  }
  error("unknown DBAuthError: {}", static_cast<int>(res.error()));
  co_return AuthError(AuthErrorCode::kAuthInternal);
}

} // namespace hogd
