#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/auth/auth_data_cache.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/auth/status.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

class AuthServer {
public:
  explicit AuthServer(IntroAuthResult intro_auth_result)
    : auth_data_cache_(1'048'576), dbauth_client_(std::move(intro_auth_result))
  {}

  x23::Async<AuthStatus<
      std::shared_ptr<std::pair<BlockHeight, std::optional<AccountAuthData>>>>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

  x23::Async<void>
  register_auth_data_updates(BlockHeight height,
                             KeyChangeBatch const &key_changes);

private:
  // If the auth data cache contains data for a ContractAddress, then that data
  // must be valid for a block height at least as recent as the current height.
  AuthDataCache auth_data_cache_;
  DBAuthClient dbauth_client_;

  // Used to stall incoming requests until the updates from the db are present,
  // and to sequence potentially out of order updates from the db.
  x23::CoroFutex<BlockHeight> current_height_futex_{0};
};

} // namespace hogd
