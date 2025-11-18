#include "hogd/hogdb/rpc_from_auth.h"

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<DBAuthResult>
HogDBAuthRpc::authdb_get_auth_data(BlockHeight height, ContractAddress address)
{
  BlockHeight current_height = ctx_.get_height_threadsafe();
  if (height != current_height) {
    co_return DBAuthResult(DBAuthError::kDBAuthStaleBlockHeight);
  }

  HogDBDAO &db = ctx_.get_db();
  HogDBStatus<AccountAuthData> status =
      db.read_address_account_auth_data(address);
  if (!status) {
    if (status.error().code() == HogDBCode::kNotFound) {
      co_return DBAuthResult(DBAuthError::kDBAuthNotFound);
    }
    co_return DBAuthResult(DBAuthError::kDBAuthInternal);
  }

  DBAuthResult result(DBAuthError::kDBAuthSuccess);
  result.auth_data() = status.value();
  co_return result;
}

x23::Async<DBAuthResultBatch>
HogDBAuthRpc::authdb_get_auth_data_batch(BlockHeight height,
                                         ContractAddressBatch addresses)
{
  DBAuthResultBatch result;
  result.reserve(addresses.size());

  BlockHeight current_height = ctx_.get_height_threadsafe();
  if (height != current_height) {
    for (auto i = 0; i < addresses.size(); i++) {
      result.emplace_back(DBAuthError::kDBAuthStaleBlockHeight);
    }
    co_return result;
  }

  auto &db = ctx_.get_db();
  for (const ContractAddress &address : addresses) {
    result.emplace_back(DBAuthError::kDBAuthSuccess);
    HogDBStatus<AccountAuthData> status =
        db.read_address_account_auth_data(address);
    if (!status) {
      if (status.error().code() == HogDBCode::kNotFound) {
        result.back().error(DBAuthError::kDBAuthNotFound);
        continue;
      }
      result.back().error(DBAuthError::kDBAuthInternal);
      continue;
    }
    result.back().auth_data() = status.value();
  }
  co_return result;
}

} // namespace hogd
