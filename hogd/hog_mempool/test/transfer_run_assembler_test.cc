#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_mempool/block_transfers.h"
#include "hogd/hog_mempool/transfer_run_assembler.h"
#include "sched_db.hh"
#include "types.hh"

namespace hogd {
namespace {

Delta
make_delta(uint8_t tag)
{
  Delta delta{.address = {}, .hog_delta = to_xdr(static_cast<int128_t>(tag))};
  delta.address[0] = tag;
  return delta;
}

HogsTransferredChunk
make_chunk(BlockHeight block, uint32_t index, uint32_t count,
           const std::vector<uint8_t> &tags)
{
  HogsTransferredChunk chunk{.block = block,
                             .chunk_index = index,
                             .chunk_count = count,
                             .deltas = {}};
  for (uint8_t tag : tags) {
    chunk.deltas.push_back(make_delta(tag));
  }
  return chunk;
}

} // namespace

TEST(TransferRunAssembler, SingleChunkRunCompletesAtOnce)
{
  TransferRunAssembler assembler;
  const std::optional<BlockTransfers> block =
      assembler.push(make_chunk(7, 0, 1, {1, 2}));
  ASSERT_TRUE(block.has_value());
  EXPECT_EQ(block->block, 7);
  ASSERT_EQ(block->delta_chunks.size(), 1U);
  EXPECT_EQ(block->delta_chunks[0].size(), 2U);
}

TEST(TransferRunAssembler, RunCompletesOnItsLastChunkInOrder)
{
  TransferRunAssembler assembler;
  EXPECT_FALSE(assembler.push(make_chunk(7, 0, 3, {1, 2})).has_value());
  EXPECT_FALSE(assembler.push(make_chunk(7, 1, 3, {3, 4})).has_value());
  const std::optional<BlockTransfers> block =
      assembler.push(make_chunk(7, 2, 3, {5}));
  ASSERT_TRUE(block.has_value());
  EXPECT_EQ(block->block, 7);
  ASSERT_EQ(block->delta_chunks.size(), 3U);
  uint8_t next = 1;
  for (const DeltaChunk &deltas : block->delta_chunks) {
    for (const Delta &delta : deltas) {
      EXPECT_EQ(delta.address[0], next);
      EXPECT_EQ(from_xdr(delta.hog_delta), static_cast<int128_t>(next));
      ++next;
    }
  }
  EXPECT_EQ(next, 6);

  // The assembler is ready for the next block, including an empty one.
  const std::optional<BlockTransfers> next_block =
      assembler.push(make_chunk(8, 0, 1, {}));
  ASSERT_TRUE(next_block.has_value());
  EXPECT_EQ(next_block->block, 8);
  ASSERT_EQ(next_block->delta_chunks.size(), 1U);
  EXPECT_TRUE(next_block->delta_chunks[0].empty());
}

} // namespace hogd
