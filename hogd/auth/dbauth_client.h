#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/status.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/db_shard_routing/sharded_client.h"
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

// One dbauth client per db shard. Auth data ('D' | address) is routed by
// address, so a lookup asks the owning shard and a subscription covers every
// shard: each forwards only the key changes for the addresses it holds.
class DBAuthClient {
public:
  DBAuthClient(dbauth_client_t &&client)
    : DBAuthClient(one_shard(std::move(client)))
  {}

  // clients[i] is shard i, so callers bind them with order_shards_by_id.
  explicit DBAuthClient(std::vector<dbauth_client_t> &&clients)
    : clients_(std::move(clients))
  {}

  size_t shard_count() const { return clients_.shard_count(); }

  x23::Async<AuthStatus<std::optional<AddressAuthData>>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

  // Introduces this auth server to every shard and returns the height they
  // agree on (-1 before any block). A divergent set is fail-stop.
  x23::Async<BlockHeight> subscribe_to_auth_data_changes(AuthCard const &card);

private:
  static std::vector<dbauth_client_t> one_shard(dbauth_client_t &&client)
  {
    std::vector<dbauth_client_t> clients;
    clients.push_back(std::move(client));
    return clients;
  }

  ShardedClient<dbauth_client_t> clients_;
};

} // namespace hogd
