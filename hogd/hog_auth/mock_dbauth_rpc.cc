#include "hogd/hog_auth/mock_dbauth_rpc.h"

#include <cassert>
#include <mutex>
#include <optional>

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {

std::optional<AddressAuthData>
MockDBAuthRpc::find_auth_data_at_height(ContractAddress address,
                                        BlockHeight height) const
{
  CHECK_EQ(height, current_height_);
  check_should_be_cached(address);
  auto it = current_auth_state_.find(address);
  if (it == current_auth_state_.end()) {
    return std::nullopt;
  }
  return it->second;
}

DBAuthResult
MockDBAuthRpc::authdb_get_auth_data(BlockHeight height, ContractAddress address)
{
  std::scoped_lock lock(mutex_);

  if (height != current_height_) {
    return DBAuthResult(DBAuthError::kDBAuthStaleBlockHeight);
  }

  auto auth_data = find_auth_data_at_height(address, height);
  if (!auth_data.has_value()) {
    return DBAuthResult(DBAuthError::kDBAuthNotFound);
  }

  DBAuthResult result(DBAuthError::kDBAuthSuccess);
  result.auth_data() = *auth_data;
  return result;
}

DBAuthResultBatch
MockDBAuthRpc::authdb_get_auth_data_batch(BlockHeight height,
                                          ContractAddressBatch const &addresses)
{
  DBAuthResultBatch result;
  result.reserve(addresses.size());

  std::scoped_lock lock(mutex_);

  if (height != current_height_) {
    for (size_t i = 0; i < addresses.size(); i++) {
      result.emplace_back(DBAuthError::kDBAuthStaleBlockHeight);
    }
    return result;
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
  return result;
}

} // namespace hogd
