#pragma once

#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "account.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/auth_data_cache.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/auth/status.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/version.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

class AuthServer {
public:
  explicit AuthServer(IntroAuthResult intro_auth_result,
                      int bucket_size = 1'048'576)
    : auth_data_cache_(bucket_size),
      dbauth_client_(std::move(intro_auth_result))
  {}

  x23::Async<AuthStatus<
      std::shared_ptr<std::pair<BlockHeight, std::optional<AccountAuthData>>>>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

  // This mentod can be called in any order.  However, it is vital for cache
  // integrity that batches of block updates are only added to the auth cache in
  // strictly increasing order, with no gaps. The CoroFutex manages this. During
  // initialization from an empty cache, if we get two blocks of updates
  // concurrently, we might process the second before the first.  In this case,
  // the first will be dropped.
  x23::Async<void>
  register_auth_data_updates(BlockHeight height,
                             KeyChangeBatch const &key_changes);

  x23::Async<void> subscribe_to_auth_data_changes(AuthCard const &card);

private:
  // If the auth data cache contains data for a ContractAddress, then that data
  // must be valid for a block height at least as recent as the current height.
  AuthDataCache auth_data_cache_;
  DBAuthClient dbauth_client_;

  // When initializing from an empty cache: whichever register_auth_data_updates
  // call arrives first will swap the current height to kInitializing.  Any
  // other pending calls will wait before proceeding.
  constexpr static BlockHeight kUninitializedHeight =
      std::numeric_limits<BlockHeight>::max();
  constexpr static BlockHeight kInitializing = kUninitializedHeight - 1;

  // Used to stall incoming requests until the updates from the db are present,
  // and to sequence potentially out of order updates from the db.
  x23::CoroFutex<BlockHeight> current_height_futex_{kUninitializedHeight};
};

class AuthDBRpc {
public:
  using rpc_service = AuthDB1;
  explicit AuthDBRpc(AuthServer &auth_server) : auth_server_(auth_server) {}

  x23::Async<void> authdb_forward_key_changes(BlockHeight height,
                                              KeyChangeBatch const &key_changes)
  {
    return auth_server_.register_auth_data_updates(height, key_changes);
  }

  x23::Async<void> subscribe_to_auth_data_changes(AuthCard const &card)
  {
    return auth_server_.subscribe_to_auth_data_changes(card);
  }

private:
  AuthServer &auth_server_;
};

static_assert(is_AuthDB1<AuthDBRpc *, x23::Async>);

class AuthDBServer : public RpcServer<AuthDBRpc> {
public:
  explicit AuthDBServer(AuthServer &auth_server) : RpcServer(auth_server) {}

  x23::Async<void> subscribe_to_auth_data_changes(std::string self_address)
  {
    auto port = RpcServer::port();
    // must start the auth db server before you can subscribe to updates
    CHECK(port != 0);
    auto addr = std::format("{}:{}", self_address, port);
    AuthCard self_card = {.protocol = version::kHogdProtocol,
                          .upid = {},
                          .address = addr.c_str()};
    return impl()->subscribe_to_auth_data_changes(self_card);
  }
};

} // namespace hogd
