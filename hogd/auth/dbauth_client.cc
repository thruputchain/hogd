#include "hogd/auth/dbauth_client.h"

#include <optional>
#include <stdexcept>
#include <utility>

#include "account.hh"
#include "auth_card.hh"
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
make_dbauth_client(const std::string &address_for_auth)
{
  x23::HostPort hostport;
  if (!hostport.parse(address_for_auth)) {
    throw std::runtime_error("Failed to parse database shard address");
  }
  return make_rpc_client<DBAuth1>(hostport);
}

} // namespace

DBAuthClient::DBAuthClient(const std::string &address_for_auth)
  : client_(make_dbauth_client(address_for_auth)())
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

x23::Async<void>
DBAuthClient::subscribe_to_auth_data_changes(AuthCard const &card)
{
  auto res = co_await client_.dbauth_intro(card);
  CHECK(res);
  co_return;
}

} // namespace hogd
