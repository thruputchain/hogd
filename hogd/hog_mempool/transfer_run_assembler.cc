#include "hogd/hog_mempool/transfer_run_assembler.h"

#include <optional>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hog_mempool/block_transfers.h"
#include "sched_db.hh"

namespace hogd {

std::optional<BlockTransfers>
TransferRunAssembler::push(HogsTransferredChunk chunk)
{
  CHECK_GT(chunk.chunk_count, 0U);
  CHECK_LT(chunk.chunk_index, chunk.chunk_count);
  if (!pending_.has_value()) {
    Pending &started = pending_.emplace();
    started.block.block = chunk.block;
    started.block.delta_chunks.resize(chunk.chunk_count);
    started.received.assign(chunk.chunk_count, false);
    started.remaining = chunk.chunk_count;
  }
  else {
    CHECK_EQ(pending_->block.block, chunk.block);
    CHECK_EQ(pending_->block.delta_chunks.size(), chunk.chunk_count);
  }
  Pending &pending = *pending_;
  CHECK(!pending.received[chunk.chunk_index]);
  pending.received[chunk.chunk_index] = true;
  pending.block.delta_chunks[chunk.chunk_index] = std::move(chunk.deltas);
  if (--pending.remaining != 0) {
    return std::nullopt;
  }
  BlockTransfers block = std::move(pending.block);
  pending_.reset();
  return block;
}

} // namespace hogd
