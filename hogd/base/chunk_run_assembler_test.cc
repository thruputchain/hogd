#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_assembler.h"
#include "hogd/base/chunked_vector.h"

namespace hogd {
namespace {

using Assembler = ChunkRunAssembler<std::uint64_t, std::vector<std::uint8_t>>;
using Items = std::vector<std::uint8_t>;

ChunkPosition
at(std::uint32_t index, std::uint32_t count)
{
  return ChunkPosition{.index = index, .count = count};
}

// The run a push completed. Fails the test and returns an empty run when the
// push did not complete one.
Assembler::Run
completed(std::optional<Assembler::Run> pushed)
{
  if (!pushed.has_value()) {
    ADD_FAILURE() << "the run did not complete on this chunk";
    return Assembler::Run{.key = 0, .parts = {}};
  }
  return std::move(*pushed);
}

// [run]'s items, read across its parts.
Items
items_of(const Assembler::Run &run)
{
  return Items(run.parts.begin(), run.parts.end());
}

} // namespace

TEST(ChunkRunAssembler, SingleChunkRunCompletesAtOnce)
{
  Assembler assembler;
  const Assembler::Run run = completed(assembler.push(7, at(0, 1), {1, 2}));
  EXPECT_EQ(run.key, 7U);
  ASSERT_EQ(run.parts.chunks().size(), 1U);
  EXPECT_EQ(items_of(run), (Items{1, 2}));
}

TEST(ChunkRunAssembler, RunCompletesOnceEveryChunkIsIn)
{
  Assembler assembler;
  EXPECT_FALSE(assembler.push(7, at(0, 3), {1, 2}).has_value());
  EXPECT_FALSE(assembler.push(7, at(1, 3), {3, 4}).has_value());
  const Assembler::Run run = completed(assembler.push(7, at(2, 3), {5}));
  EXPECT_EQ(run.key, 7U);
  ASSERT_EQ(run.parts.chunks().size(), 3U);
  EXPECT_EQ(run.parts.size(), 5U);
  EXPECT_EQ(items_of(run), (Items{1, 2, 3, 4, 5}));

  // The assembler is ready for the next run, including an empty one.
  const Assembler::Run next = completed(assembler.push(8, at(0, 1), {}));
  EXPECT_EQ(next.key, 8U);
  ASSERT_EQ(next.parts.chunks().size(), 1U);
  EXPECT_TRUE(next.parts.empty());
}

TEST(ChunkRunAssembler, ChunksLandInTheirSlotsWhateverTheirArrivalOrder)
{
  Assembler assembler;
  // The run's length and key come from whichever chunk arrives first.
  EXPECT_FALSE(assembler.push(7, at(2, 3), {5}).has_value());
  EXPECT_FALSE(assembler.push(7, at(0, 3), {1, 2}).has_value());
  const Assembler::Run run = completed(assembler.push(7, at(1, 3), {3, 4}));
  EXPECT_EQ(run.key, 7U);
  ASSERT_EQ(run.parts.chunks().size(), 3U);
  EXPECT_EQ(run.parts.chunks()[2], (Items{5}));
  EXPECT_EQ(items_of(run), (Items{1, 2, 3, 4, 5}));
}

TEST(ChunkRunAssembler, AnEmptyChunkStillCountsTowardTheRun)
{
  Assembler assembler;
  EXPECT_FALSE(assembler.push(7, at(1, 2), {}).has_value());
  const Assembler::Run run = completed(assembler.push(7, at(0, 2), {1}));
  ASSERT_EQ(run.parts.chunks().size(), 2U);
  EXPECT_EQ(items_of(run), (Items{1}));
  EXPECT_TRUE(run.parts.chunks()[1].empty());
}

} // namespace hogd
