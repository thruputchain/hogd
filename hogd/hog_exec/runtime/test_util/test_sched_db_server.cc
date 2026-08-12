#include "hogd/hog_exec/runtime/test_util/test_sched_db_server.h"

#include <map>
#include <mutex>

#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {

void
TestSchedDBServer::wait_for_hog_transferred_height(BlockHeight height)
{
  std::unique_lock<std::mutex> lock(mtx_);
  cv_.wait(lock, [this, height] { return hogs_transferred_.contains(height); });
}

void
TestSchedDBServer::wait_for_changed_accounts_height(BlockHeight height)
{
  std::unique_lock<std::mutex> lock(mtx_);
  cv_.wait(lock, [this, height] { return changed_accounts_.contains(height); });
}

std::map<ContractAddress, int128_t>
TestSchedDBServer::get_hogs_transferred(BlockHeight height)
{
  std::unique_lock<std::mutex> lock(mtx_);
  return hogs_transferred_.at(height);
}

ContractAddressBatch
TestSchedDBServer::get_changed_accounts(BlockHeight height)
{
  std::unique_lock<std::mutex> lock(mtx_);
  return changed_accounts_.at(height);
}
} // namespace hogd
