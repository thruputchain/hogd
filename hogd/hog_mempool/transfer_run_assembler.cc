#include "hogd/hog_mempool/transfer_run_assembler.h"

#include <cstdint>
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
  if (chunk.chunk_index == 0) {
    CHECK(!pending_.has_value());
    pending_.emplace(Pending{.block = BlockTransfers{.block = chunk.block,
                                                     .delta_chunks = {}},
                             .chunk_count = chunk.chunk_count});
    pending_->block.delta_chunks.reserve(chunk.chunk_count);
  }
  else {
    CHECK(pending_.has_value());
    CHECK_EQ(pending_->block.block, chunk.block);
    CHECK_EQ(pending_->chunk_count, chunk.chunk_count);
    CHECK_EQ(pending_->block.delta_chunks.size(), chunk.chunk_index);
  }
  pending_->block.delta_chunks.push_back(std::move(chunk.deltas));
  if (chunk.chunk_index + 1 != chunk.chunk_count) {
    return std::nullopt;
  }
  BlockTransfers block = std::move(pending_->block);
  pending_.reset();
  return block;
}

} // namespace hogd
