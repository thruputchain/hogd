#include "hogd/hogdb/mock_authdb_rpc.h"

#include <mutex>

#include "auth_rpc.hh"
#include "x23/async.h"

namespace hogd {

void
MockAuthDBRpc::wait_for_changes(BlockHeight height)
{
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, height] { return received_changes_.contains(height); });
}

x23::Async<void>
MockAuthDBRpc::authdb_forward_key_changes(BlockHeight height,
                                          KeyChangeBatch key_changes)
{
  std::lock_guard<std::mutex> lock(mutex_);

  // Append to existing changes for this height, or create new entry
  auto it = received_changes_.find(height);
  if (it != received_changes_.end()) {
    // Append new changes to existing batch
    it->second.append_range(key_changes);
  }
  else {
    received_changes_[height] = std::move(key_changes);
  }

  cv_.notify_all();
  co_return;
}

} // namespace hogd
