#include "hogd/db_shard_routing/shard_router.h"

#include <cstdint>
#include <set>

#include "gtest/gtest.h"

#include "types.hh" // AddressAndKey, ContractAddress

namespace hogd {
namespace {

// Builds a distinct address from a seed by writing the seed into the bytes.
ContractAddress make_address(uint64_t seed)
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
    EXPECT_EQ(shard_of_address(make_address(seed), /*shard_count=*/1), 0u);
  }
}

// Routing is deterministic within a process: same address -> same shard.
TEST(ShardRouterTest, Deterministic)
{
  ContractAddress const addr = make_address(0xABCDEF);
  size_t const first = shard_of_address(addr, /*shard_count=*/8);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(shard_of_address(addr, /*shard_count=*/8), first);
  }
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
  EXPECT_GT(hit.size(), 1u);
}

} // namespace
} // namespace hogd
