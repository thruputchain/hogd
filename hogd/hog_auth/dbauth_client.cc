#include "hogd/hog_auth/dbauth_client.h"

#include <optional>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/hog_auth/status.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<AuthStatus<std::optional<AddressAuthData>>>
DBAuthClient::lookup_auth_data(BlockHeight height, ContractAddress address)
{
  auto res = co_await client_.authdb_get_auth_data(height, address);

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
  DBAuthIntroResult res = co_await client_.dbauth_intro(card);
  CHECK_EQ(res.outcome(), DBAuthIntroOutcome::kIntroSuccess);
  co_return res.current_height();
}

} // namespace hogd
