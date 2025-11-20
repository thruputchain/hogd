#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/auth/auth_data_cache.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/auth/status.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class AuthServer {
public:
  explicit AuthServer(IntroAuthResult intro_auth_result)
    : auth_data_cache_(1'048'576), dbauth_client_(std::move(intro_auth_result))
  {}

  x23::Async<AuthStatus<
      std::shared_ptr<std::pair<BlockHeight, std::optional<AccountAuthData>>>>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

  void register_auth_data_updates(BlockHeight height,
                                  KeyChangeBatch const &key_changes);

private:
  // If the auth data cache contains data for a ContractAddress, then that data
  // must be valid for a block height at least as recent as the current height.
  AuthDataCache auth_data_cache_;
  DBAuthClient dbauth_client_;

  std::atomic<BlockHeight> current_height_ = 0;

  // Not strictly required if the db and network behave well.
  // This gives a guarantee that we won't inadvertently process key change
  // updates in a bad order and leave the auth data cache in an inconsistent
  // state.
  std::mutex key_changes_mutex_;
  std::condition_variable key_changes_cv_;
};

} // namespace hogd
