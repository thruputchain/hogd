#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

#include "hogd/base/chunk_items.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {
namespace {

using Items = std::vector<std::uint32_t>;

} // namespace

static_assert(x23::serializes_like<ChunkItems<std::uint32_t>,
                                   x23::XdrVec<std::uint32_t>>);

TEST(ChunkItems, SerializesLikeTheVectorOfItsItems)
{
  const Items first{1, 2, 3};
  const Items second{};
  const Items third{4};
  const std::vector<std::span<const std::uint32_t>> pieces{first, second,
                                                           third};
  const ChunkItems<std::uint32_t> items(pieces);
  const x23::XdrVec<std::uint32_t> vector{1, 2, 3, 4};
  EXPECT_EQ(items.size(), 4U);
  EXPECT_EQ(xdr_size(items), x23::xdr_size(vector));
  EXPECT_EQ(x23::serialize(items), x23::serialize(vector));
}

TEST(ChunkItems, SerializesVariableSizeItemsLikeTheirVector)
{
  using Inner = x23::XdrVec<std::uint32_t>;
  const std::vector<Inner> first{Inner{1, 2}, Inner{}};
  const std::vector<Inner> second{Inner{3, 4, 5}};
  const std::vector<std::span<const Inner>> pieces{first, second};
  const ChunkItems<Inner> items(pieces);
  const x23::XdrVec<Inner> vector{Inner{1, 2}, Inner{}, Inner{3, 4, 5}};
  EXPECT_EQ(xdr_size(items), x23::xdr_size(vector));
  EXPECT_EQ(x23::serialize(items), x23::serialize(vector));
}

TEST(ChunkItems, EmptyChunkSerializesLikeAnEmptyVector)
{
  const std::vector<std::span<const std::uint32_t>> pieces;
  const ChunkItems<std::uint32_t> items(pieces);
  EXPECT_EQ(x23::serialize(items),
            x23::serialize(x23::XdrVec<std::uint32_t>{}));
}

} // namespace hogd
