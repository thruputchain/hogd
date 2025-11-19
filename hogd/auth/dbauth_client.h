#pragma once

#include <optional>

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/auth/status.h"
#include "hogd/base/rpc_client.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class DBAuthClient {
public:
  DBAuthClient(IntroAuthResult intro_auth_result);

  x23::Async<AuthStatus<std::optional<AccountAuthData>>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

private:
  using dbauth_client_t = decltype(make_rpc_client<DBAuth1>(0));

  // Sharded DB is not yet implemented.
  dbauth_client_t client_;
};

} // namespace hogd
