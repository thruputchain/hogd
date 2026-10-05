#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include "hogd/base/chunked_vector.h"
#include "hogd/base/types.h"

namespace hogd {
namespace {

using Chunk = std::vector<std::uint32_t>;
using Chunked = ChunkedVector<Chunk>;

static_assert(std::forward_iterator<Chunked::const_iterator>);

Chunk
collect(const Chunked &chunked)
{
  return Chunk(chunked.begin(), chunked.end());
}

} // namespace

TEST(ChunkedVector, IteratesTheItemsOfEveryChunkInOrder)
{
  const Chunked chunked(std::vector<Chunk>{{1, 2}, {3}, {4, 5, 6}});
  EXPECT_EQ(chunked.size(), 6U);
  EXPECT_FALSE(chunked.empty());
  EXPECT_EQ(collect(chunked), (Chunk{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(std::distance(chunked.begin(), chunked.end()), 6);
}

TEST(ChunkedVector, APlainChunkIsASequenceOfOne)
{
  const Chunked chunked = Chunk{1, 2, 3};
  EXPECT_EQ(chunked.size(), 3U);
  EXPECT_EQ(chunked.chunks().size(), 1U);
  EXPECT_EQ(collect(chunked), (Chunk{1, 2, 3}));
}

TEST(ChunkedVector, SkipsEmptyChunksWhereverTheyAre)
{
  const Chunked chunked(std::vector<Chunk>{{}, {1}, {}, {}, {2, 3}, {}});
  EXPECT_EQ(chunked.size(), 3U);
  EXPECT_EQ(collect(chunked), (Chunk{1, 2, 3}));
  EXPECT_EQ(*chunked.begin(), 1U);
}

TEST(ChunkedVector, NoItemsMeansBeginIsEnd)
{
  const Chunked none;
  EXPECT_TRUE(none.empty());
  EXPECT_EQ(none.size(), 0U);
  EXPECT_EQ(none.begin(), none.end());

  const Chunked only_empty_chunks(std::vector<Chunk>{{}, {}});
  EXPECT_TRUE(only_empty_chunks.empty());
  EXPECT_EQ(only_empty_chunks.begin(), only_empty_chunks.end());
  EXPECT_EQ(only_empty_chunks.chunks().size(), 2U);
}

TEST(ChunkedVector, IndexesAcrossTheChunks)
{
  const Chunked chunked(std::vector<Chunk>{{10, 11}, {}, {12}, {13, 14}});
  for (size_t i = 0; i < chunked.size(); ++i) {
    EXPECT_EQ(chunked[i], 10U + i) << i;
  }
}

TEST(ChunkedVector, IteratorAtLandsOnTheItemAndStepsOnFromThere)
{
  const Chunked chunked(std::vector<Chunk>{{10, 11}, {}, {12}, {13, 14}});
  for (size_t i = 0; i < chunked.size(); ++i) {
    Chunked::const_iterator it = chunked.iterator_at(i);
    EXPECT_EQ(*it, 10U + i) << i;
    // Stepping from a positioned iterator reaches the same end as begin().
    Chunk rest(it, chunked.end());
    EXPECT_EQ(rest.size(), chunked.size() - i) << i;
    EXPECT_EQ(rest.front(), 10U + i) << i;
    EXPECT_EQ(rest.back(), 14U) << i;
  }
  EXPECT_EQ(chunked.iterator_at(chunked.size()), chunked.end());
  EXPECT_EQ(chunked.iterator_at(0), chunked.begin());
}

TEST(ChunkedVector, StillExposesTheChunks)
{
  const Chunked chunked(std::vector<Chunk>{{1, 2}, {3}});
  ASSERT_EQ(chunked.chunks().size(), 2U);
  EXPECT_EQ(chunked.chunks()[0], (Chunk{1, 2}));
  EXPECT_EQ(chunked.chunks()[1], (Chunk{3}));
}

TEST(ChunkedVector, WorksWithStandardAlgorithms)
{
  const Chunked chunked(std::vector<Chunk>{{5, 1}, {}, {4, 2, 3}});
  EXPECT_EQ(*std::ranges::max_element(chunked), 5U);
  EXPECT_EQ(std::ranges::count_if(chunked,
                                  [](std::uint32_t v) { return v > 2; }),
            3);
  EXPECT_NE(std::ranges::find(chunked, 3U), chunked.end());
  EXPECT_EQ(std::ranges::find(chunked, 9U), chunked.end());
}

} // namespace hogd
