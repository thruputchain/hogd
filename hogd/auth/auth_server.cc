#include "hogd/auth/auth_server.h"

#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/auth/status.h"
#include "hogd/base/log.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<AuthStatus<
    std::shared_ptr<std::pair<BlockHeight, std::optional<AccountAuthData>>>>>
AuthServer::lookup_auth_data(BlockHeight height, ContractAddress address)
{
  BlockHeight current_height = current_height_.load();
  if (height > current_height) {
    panic("Waiting on incoming data is not yet implemented");
  }
  if (height < current_height) {
    co_return AuthError(AuthErrorCode::kAuthStaleBlockHeight);
  }

  auto auth_data = auth_data_cache_.get(address);
  if (auth_data != nullptr) {
    // Auth data cache has processed all updates from some initial height up to
    // and including current_height. That means that if the data is present
    // there, and it's not from a block in the future, then it must be valid for
    // current_height.
    if (auth_data->first <= current_height) {
      co_return auth_data;
    }
  }

  auto db_auth_data = co_await dbauth_client_.lookup_auth_data(height, address);
  if (!db_auth_data.has_value()) {
    co_return db_auth_data.error();
  }

  auto entry =
      std::make_shared<std::pair<BlockHeight, std::optional<AccountAuthData>>>(
          height, db_auth_data.value());

  auth_data_cache_.put(address, entry);

  co_return entry;
}

void
AuthServer::register_auth_data_updates(BlockHeight height,
                                       KeyChangeBatch const &key_changes)
{
  std::unique_lock lock(key_changes_mutex_);
  key_changes_cv_.wait(lock, [this, height] {
    return height == current_height_.load() + 1;
  });
  CHECK(height == current_height_.load() + 1);

  for (const auto &key_change : key_changes) {
    if (key_change.auth_data == nullptr) {
      auto entry = std::make_shared<
          std::pair<BlockHeight, std::optional<AccountAuthData>>>(height,
                                                                  std::nullopt);
      auth_data_cache_.put(key_change.address, entry);
      continue;
    }
    auto entry = std::make_shared<
        std::pair<BlockHeight, std::optional<AccountAuthData>>>(
        height, *key_change.auth_data);
    auth_data_cache_.put(key_change.address, entry);
  }
  current_height_.store(height);
  key_changes_cv_.notify_all();
}

} // namespace hogd
