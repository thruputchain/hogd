#include "hogd/auth/mock_dbauth_rpc.h"

#include <cassert>
#include <mutex>
#include <optional>

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

std::optional<AccountAuthData>
MockDBAuthRpc::find_auth_data_at_height(ContractAddress address,
                                        BlockHeight height) const
{
  assert(height == current_height_);
  check_should_be_cached(address);
  auto it = current_auth_state_.find(address);
  if (it == current_auth_state_.end()) {
    return std::nullopt;
  }
  return it->second;
}

x23::Async<DBAuthResult>
MockDBAuthRpc::authdb_get_auth_data(BlockHeight height, ContractAddress address)
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (height != current_height_) {
    co_return DBAuthResult(DBAuthError::kDBAuthStaleBlockHeight);
  }

  auto auth_data = find_auth_data_at_height(address, height);
  if (!auth_data.has_value()) {
    co_return DBAuthResult(DBAuthError::kDBAuthNotFound);
  }

  DBAuthResult result(DBAuthError::kDBAuthSuccess);
  result.auth_data() = *auth_data;
  co_return result;
}

x23::Async<DBAuthResultBatch>
MockDBAuthRpc::authdb_get_auth_data_batch(BlockHeight height,
                                          ContractAddressBatch addresses)
{
  DBAuthResultBatch result;
  result.reserve(addresses.size());

  std::lock_guard<std::mutex> lock(mutex_);

  if (height != current_height_) {
    for (size_t i = 0; i < addresses.size(); i++) {
      result.emplace_back(DBAuthError::kDBAuthStaleBlockHeight);
    }
    co_return result;
  }

  for (const ContractAddress &address : addresses) {
    auto auth_data = find_auth_data_at_height(address, height);
    if (!auth_data.has_value()) {
      result.emplace_back(DBAuthError::kDBAuthNotFound);
    }
    else {
      result.emplace_back(DBAuthError::kDBAuthSuccess);
      result.back().auth_data() = *auth_data;
    }
  }
  co_return result;
}

} // namespace hogd
