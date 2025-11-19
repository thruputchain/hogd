#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include "account.hh"
#include "hogd/auth/auth_data_cache.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/auth/status.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class AuthServer {
public:
  x23::Async<AuthStatus<
      std::shared_ptr<std::pair<BlockHeight, std::optional<AccountAuthData>>>>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

private:
  // If the auth data cache contains data for a ContractAddress, then that data
  // must be valid for a block height at least as recent as the current height.

  AuthDataCache auth_data_cache_;
  DBAuthClient dbauth_client_;

  std::atomic<BlockHeight> current_height_ = 0;
};

} // namespace hogd
