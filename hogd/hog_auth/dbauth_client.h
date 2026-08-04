#pragma once

#include <optional>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/hog_auth/status.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/sockio.h"

namespace hogd {

using dbauth_client_t = rpc_client_t<DBAuth1>;

// address_for_auth comes from the mempool's intro response: a malformed one
// means the peer sent us garbage, so there is nothing to fall back to.
inline x23::Async<ErrnoStatus<dbauth_client_t>>
make_dbauth_client(const std::string &address_for_auth)
{
  x23::HostPort host_port;
  if (!host_port.parse(address_for_auth)) {
    panic("Failed to parse database shard address: {}", address_for_auth);
  }
  return make_rpc_client<DBAuth1>(host_port);
}

class DBAuthClient {
public:
  DBAuthClient(dbauth_client_t &&client) : client_(std::move(client)) {}

  x23::Async<AuthStatus<std::optional<AddressAuthData>>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

  x23::Async<BlockHeight> subscribe_to_auth_data_changes(AuthCard const &card);

private:
  // Sharded DB is not yet implemented.
  dbauth_client_t client_;
};

} // namespace hogd
