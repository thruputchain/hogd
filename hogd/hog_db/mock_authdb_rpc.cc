#include "hogd/hog_db/mock_authdb_rpc.h"

#include <mutex>
#include <optional>
#include <utility>

#include "auth_rpc.hh"
#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_assembler.h"
#include "types.hh"

namespace hogd {

void
MockAuthDBRpc::wait_for_changes(BlockHeight height)
{
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, height] { return received_.contains(height); });
}

void
MockAuthDBRpc::authdb_forward_key_changes_chunk(KeyChangeChunk chunk)
{
  std::optional<ChunkRunAssembler<BlockHeight, KeyChangeBatch>::Run> run =
      assembler_.push(chunk.height,
                      ChunkPosition{.index = chunk.chunk_index,
                                    .count = chunk.chunk_count},
                      std::move(chunk.changes));
  if (!run.has_value()) {
    return;
  }
  KeyChangeBatch changes;
  changes.reserve(run->parts.size());
  for (const KeyChange &change : run->parts) {
    changes.push_back(change);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  received_[run->key] = std::move(changes);
  cv_.notify_all();
}

} // namespace hogd
