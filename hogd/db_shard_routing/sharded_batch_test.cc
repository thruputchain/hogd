#include "hogd/db_shard_routing/sharded_batch.h"

#include <cstddef>
#include <vector>

#include "gtest/gtest.h"

namespace hogd {
namespace {

// Route an int to a shard by value % shard_count.
auto by_mod = [](int v, size_t n) { return static_cast<size_t>(v) % n; };

// Helper: given a split, produce per-shard results by applying `f` to each
// item in each shard's sub-batch -- mimicking what a real per-shard RPC call
// returns, in the order the shard received the items.
template <typename F>
std::vector<std::vector<int>>
make_per_shard_results(ShardSplit<std::vector<int>> const &split, F f)
{
  std::vector<std::vector<int>> per_shard(split.shard_count());
  for (size_t s = 0; s < split.shard_count(); ++s) {
    for (int item : split.batches[s]) {
      per_shard[s].push_back(f(item));
    }
  }
  return per_shard;
}

TEST(ShardedBatchTest, RoutesEachItemToItsShard)
{
  std::vector<int> items{0, 1, 2, 3, 4, 5, 6, 7, 8};
  auto split = split_by_shard(items, /*shard_count=*/3, by_mod);

  EXPECT_EQ(split.total, 9u);
  EXPECT_EQ(split.batches[0], (std::vector<int>{0, 3, 6}));
  EXPECT_EQ(split.batches[1], (std::vector<int>{1, 4, 7}));
  EXPECT_EQ(split.batches[2], (std::vector<int>{2, 5, 8}));
}

TEST(ShardedBatchTest, TracksOriginIndices)
{
  std::vector<int> items{10, 11, 12, 13, 14};
  auto split = split_by_shard(items, /*shard_count=*/2, by_mod);

  // Even values -> shard 0, odd -> shard 1, both in input order.
  EXPECT_EQ(split.origin[0], (std::vector<size_t>{0, 2, 4})); // 10,12,14
  EXPECT_EQ(split.origin[1], (std::vector<size_t>{1, 3}));    // 11,13
}

// The correctness-critical property: results reassemble into input order even
// when consecutive items land on different shards.
TEST(ShardedBatchTest, GatherRestoresInputOrderInterlaced)
{
  std::vector<int> items{0, 1, 2, 3, 4, 5, 6, 7};
  auto split = split_by_shard(items, /*shard_count=*/4, by_mod);

  auto per_shard = make_per_shard_results(split, [](int v) { return v * 10; });
  auto out = gather_positional(split, per_shard);

  ASSERT_EQ(out.size(), items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    EXPECT_EQ(out[i], items[i] * 10) << "at index " << i;
  }
}

TEST(ShardedBatchTest, SingleShardPreservesOrder)
{
  std::vector<int> items{5, 9, 2, 7, 1};
  auto split = split_by_shard(items, /*shard_count=*/1, by_mod);

  EXPECT_EQ(split.batches[0], items);
  auto per_shard = make_per_shard_results(split, [](int v) { return v + 100; });
  auto out = gather_positional(split, per_shard);

  ASSERT_EQ(out.size(), items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    EXPECT_EQ(out[i], items[i] + 100);
  }
}

TEST(ShardedBatchTest, EmptyInput)
{
  std::vector<int> items{};
  auto split = split_by_shard(items, /*shard_count=*/3, by_mod);

  EXPECT_EQ(split.total, 0u);
  for (size_t s = 0; s < 3; ++s) {
    EXPECT_TRUE(split.shard_empty(s));
  }
  auto per_shard = make_per_shard_results(split, [](int v) { return v; });
  auto out = gather_positional(split, per_shard);
  EXPECT_TRUE(out.empty());
}

// Some shards receive nothing; reassembly must still be correct.
TEST(ShardedBatchTest, HandlesEmptyShards)
{
  // With shard_count 5 and these values, only shards 1 and 3 are hit.
  std::vector<int> items{1, 3, 6, 8, 11};
  auto split = split_by_shard(items, /*shard_count=*/5, by_mod);

  EXPECT_TRUE(split.shard_empty(0));
  EXPECT_TRUE(split.shard_empty(2));
  EXPECT_TRUE(split.shard_empty(4));
  EXPECT_FALSE(split.shard_empty(1)); // 1, 6, 11
  EXPECT_FALSE(split.shard_empty(3)); // 3, 8

  auto per_shard = make_per_shard_results(split, [](int v) { return v * 2; });
  auto out = gather_positional(split, per_shard);
  ASSERT_EQ(out.size(), items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    EXPECT_EQ(out[i], items[i] * 2);
  }
}

TEST(ShardedBatchTest, AllItemsOnOneNonZeroShard)
{
  std::vector<int> items{2, 5, 8, 11};
  // Route everything to shard 2 regardless of value.
  auto split = split_by_shard(items, /*shard_count=*/3,
                              [](int, size_t) { return size_t{2}; });

  EXPECT_TRUE(split.shard_empty(0));
  EXPECT_TRUE(split.shard_empty(1));
  EXPECT_EQ(split.batches[2], items);

  auto per_shard = make_per_shard_results(split, [](int v) { return v - 1; });
  auto out = gather_positional(split, per_shard);
  ASSERT_EQ(out.size(), items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    EXPECT_EQ(out[i], items[i] - 1);
  }
}

} // namespace
} // namespace hogd
