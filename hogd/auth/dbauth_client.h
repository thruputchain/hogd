#pragma once

#include <optional>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/status.h"
#include "hogd/base/rpc_client.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/sockio.h"

namespace hogd {

using dbauth_client_t = decltype(make_rpc_client<DBAuth1>(x23::HostPort{})());

class DBAuthClient {
public:
  DBAuthClient(dbauth_client_t &&client) : client_(std::move(client)) {}

  x23::Async<AuthStatus<std::optional<AddressAuthData>>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

  x23::Async<void> subscribe_to_auth_data_changes(AuthCard const &card);

private:
  // Sharded DB is not yet implemented.
  dbauth_client_t client_;
};

} // namespace hogd
