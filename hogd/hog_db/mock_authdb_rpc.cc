#include "hogd/hog_db/mock_authdb_rpc.h"

#include <mutex>
#include <utility>

#include "auth_rpc.hh"
#include "types.hh"

namespace hogd {

void
MockAuthDBRpc::wait_for_changes(BlockHeight height)
{
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, height] {
    auto it = received_.find(height);
    return it != received_.end() && it->second.complete;
  });
}

void
MockAuthDBRpc::authdb_forward_key_changes_chunk(KeyChangeChunk chunk)
{
  std::lock_guard<std::mutex> lock(mutex_);

  Received &received = received_[chunk.height];
  received.changes.append_range(chunk.changes);
  if (++received.chunks == chunk.chunk_count) {
    received.complete = true;
    cv_.notify_all();
  }
}

} // namespace hogd
