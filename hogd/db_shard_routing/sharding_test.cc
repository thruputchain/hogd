#include "hogd/db_shard_routing/sharding.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "gtest/gtest.h"

#include "types.hh" // AddressAndKey, ContractAddress
#include "x23/x23/serialize.h"
#include "x23/x23/vec.h"

namespace hogd {
namespace {

// ---------------------------------------------------------------------------
// Routing (shard_of_address / shard_of_key)
// ---------------------------------------------------------------------------

// Builds a distinct address from a seed by writing the seed into the bytes.
ContractAddress
make_address(uint64_t seed)
{
  ContractAddress addr{};
  for (size_t i = 0; i < sizeof(seed); ++i) {
    addr[i] = static_cast<uint8_t>((seed >> (8 * i)) & 0xFF);
  }
  return addr;
}

// A single shard: every address maps to shard 0.
TEST(ShardRouterTest, SingleShardAlwaysZero)
{
  for (uint64_t seed = 0; seed < 256; ++seed) {
    EXPECT_EQ(shard_of_address(make_address(seed), /*shard_count=*/1), 0U);
  }
}

// Routing is deterministic within a process: same address -> same shard.
TEST(ShardRouterTest, Deterministic)
{
  ContractAddress const addr = make_address(0xABCDEF);
  EXPECT_EQ(shard_of_address(addr, /*shard_count=*/8),
            shard_of_address(addr, /*shard_count=*/8));
}

// The shard index is always in [0, shard_count).
TEST(ShardRouterTest, InRange)
{
  for (size_t shard_count = 1; shard_count <= 16; ++shard_count) {
    for (uint64_t seed = 0; seed < 512; ++seed) {
      EXPECT_LT(shard_of_address(make_address(seed), shard_count), shard_count);
    }
  }
}

// All of an address's items co-locate: shard_of_key routes purely by the
// account address, so it must agree with shard_of_address regardless of key.
TEST(ShardRouterTest, CoLocationByAddress)
{
  for (uint64_t seed = 0; seed < 256; ++seed) {
    ContractAddress const addr = make_address(seed);
    AddressAndKey ak{};
    ak.address = addr;
    EXPECT_EQ(shard_of_key(ak, /*shard_count=*/8),
              shard_of_address(addr, /*shard_count=*/8));
  }
}

// Sanity: with more than one shard, a spread of addresses lands on more than
// one shard (the router is not degenerate). Not a distribution-quality claim.
TEST(ShardRouterTest, SpreadsAcrossShards)
{
  std::set<size_t> hit;
  for (uint64_t seed = 0; seed < 1000; ++seed) {
    hit.insert(shard_of_address(make_address(seed), /*shard_count=*/8));
  }
  EXPECT_GT(hit.size(), 1U);
}

// ---------------------------------------------------------------------------
// Cross-shard batch split / gather
// ---------------------------------------------------------------------------

// Route an int to a shard by value % shard_count.
auto by_mod = [](int v, size_t n) { return static_cast<size_t>(v) % n; };

// Materialize shard s's sub-batch the old (copying) way, by gathering the
// source items at the shard's origin indices. The zero-copy split no longer
// stores these; tests reconstruct them to assert on routing.
std::vector<int>
shard_items(ShardSplit<std::vector<int>> const &split,
            std::vector<int> const &items, size_t s)
{
  std::vector<int> out;
  for (size_t idx : split.origin[s]) {
    out.push_back(items[idx]);
  }
  return out;
}

// Helper: given a split, produce per-shard results by applying `f` to each
// item in each shard's sub-batch -- mimicking what a real per-shard RPC call
// returns, in the order the shard received the items.
template<typename F>
std::vector<std::vector<int>>
make_per_shard_results(ShardSplit<std::vector<int>> const &split,
                       std::vector<int> const &items, F f)
{
  std::vector<std::vector<int>> per_shard(split.shard_count());
  for (size_t s = 0; s < split.shard_count(); ++s) {
    for (size_t idx : split.origin[s]) {
      per_shard[s].push_back(f(items[idx]));
    }
  }
  return per_shard;
}

TEST(ShardedBatchTest, RoutesEachItemToItsShard)
{
  std::vector<int> items{0, 1, 2, 3, 4, 5, 6, 7, 8};
  auto split = split_by_shard(items, /*shard_count=*/3, by_mod);

  EXPECT_EQ(split.total, 9U);
  EXPECT_EQ(shard_items(split, items, 0), (std::vector<int>{0, 3, 6}));
  EXPECT_EQ(shard_items(split, items, 1), (std::vector<int>{1, 4, 7}));
  EXPECT_EQ(shard_items(split, items, 2), (std::vector<int>{2, 5, 8}));
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

  auto per_shard =
      make_per_shard_results(split, items, [](int v) { return v * 10; });
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

  EXPECT_EQ(shard_items(split, items, 0), items);
  auto per_shard =
      make_per_shard_results(split, items, [](int v) { return v + 100; });
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

  EXPECT_EQ(split.total, 0U);
  for (size_t s = 0; s < 3; ++s) {
    EXPECT_TRUE(split.shard_empty(s));
  }
  auto per_shard =
      make_per_shard_results(split, items, [](int v) { return v; });
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

  auto per_shard =
      make_per_shard_results(split, items, [](int v) { return v * 2; });
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
  EXPECT_EQ(shard_items(split, items, 2), items);

  auto per_shard =
      make_per_shard_results(split, items, [](int v) { return v - 1; });
  auto out = gather_positional(split, per_shard);
  ASSERT_EQ(out.size(), items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    EXPECT_EQ(out[i], items[i] - 1);
  }
}

// The point of the zero-copy split: a shard's ShardGatherView must serialize
// byte-for-byte like the concrete Vec of that shard's items would have. This is
// what lets the view be handed to an RPC in place of a materialized sub-batch.
TEST(ShardedBatchTest, GatherViewSerializesLikeMaterializedVec)
{
  x23::Vec<int32_t> items{0, 1, 2, 3, 4, 5, 6, 7, 8};
  auto split = split_by_shard(items, /*shard_count=*/3, by_mod);

  for (size_t s = 0; s < split.shard_count(); ++s) {
    // Materialize the shard's sub-batch the old (copying) way.
    x23::Vec<int32_t> materialized;
    for (size_t idx : split.origin[s]) {
      materialized.push_back(items[idx]);
    }
    auto view = split.shard_view(items, s);

    EXPECT_EQ(xdr_size(view), xdr_size(materialized));
    EXPECT_EQ(x23::serialize(view), x23::serialize(materialized));
  }
}

} // namespace
} // namespace hogd
