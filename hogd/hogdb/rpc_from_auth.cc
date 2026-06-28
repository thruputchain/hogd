#include "hogd/hogdb/rpc_from_auth.h"

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

BlockHeight
HogDBAuthRpc::get_expected_query_height()
{
  // We want to make sure we don't answer stale queries incorrectly.
  return ctx_.get_height_threadsafe();
}

x23::Async<DBAuthResult>
HogDBAuthRpc::authdb_get_auth_data(BlockHeight height, ContractAddress address)
{
  if (height != get_expected_query_height()) {
    info("Auth data is stale at height {}, expected height {}, returning stale "
         "block height error",
         height, get_expected_query_height());
    co_return DBAuthResult(DBAuthError::kDBAuthStaleBlockHeight);
  }

  HogDBDAO &db = ctx_.get_db();
  HogDBStatus<AddressAuthData> status = db.read_address_auth_data(address);
  if (!status) {
    if (status.error().code() == HogDBCode::kNotFound) {
      co_return DBAuthResult(DBAuthError::kDBAuthNotFound);
    }
    co_return DBAuthResult(DBAuthError::kDBAuthInternal);
  }

  // The height check above is against current_committed_height_, which the
  // scheduler advances only after BlockTX::commit() has applied the block to
  // the live DAO. In that window the row read here can already belong to
  // height+1, even though the query was accepted for [height]. Reject rather
  // than return a not-yet-published auth row (issue #1725).
  if (status.value().last_written_height > height) {
    co_return DBAuthResult(DBAuthError::kDBAuthStaleBlockHeight);
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

  if (height != get_expected_query_height()) {
    for (size_t i = 0; i < addresses.size(); i++) {
      result.emplace_back(DBAuthError::kDBAuthStaleBlockHeight);
    }
    co_return result;
  }

  auto &db = ctx_.get_db();
  for (const ContractAddress &address : addresses) {
    result.emplace_back(DBAuthError::kDBAuthSuccess);
    HogDBStatus<AddressAuthData> status = db.read_address_auth_data(address);
    if (!status) {
      if (status.error().code() == HogDBCode::kNotFound) {
        result.back().error(DBAuthError::kDBAuthNotFound);
        continue;
      }
      result.back().error(DBAuthError::kDBAuthInternal);
      continue;
    }
    // Reject any row already written by a height newer than the accepted query
    // height; see authdb_get_auth_data above and issue #1725.
    if (status.value().last_written_height > height) {
      result.back().error(DBAuthError::kDBAuthStaleBlockHeight);
      continue;
    }
    result.back().auth_data() = status.value();
  }
  co_return result;
}

} // namespace hogd
