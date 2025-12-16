#include <gtest/gtest.h>

#include "hogd/storage_item/replay_cache.h"
#include "transaction.hh"

namespace hogd {

TEST(ReplayCache, Basic)
{

  ReplayCache cache(10);

  cache.add(Uniquifier{.block_expiry_height = 11, .nonce = 11});
  cache.add(Uniquifier{.block_expiry_height = 12, .nonce = 12});
  cache.add(Uniquifier{.block_expiry_height = 14, .nonce = 14});

  EXPECT_FALSE(
      cache.can_accept(Uniquifier{.block_expiry_height = 9, .nonce = 9}));

  EXPECT_FALSE(
      cache.can_accept(Uniquifier{.block_expiry_height = 9, .nonce = 9}));
  EXPECT_TRUE(
      cache.can_accept(Uniquifier{.block_expiry_height = 10, .nonce = 10}));
  EXPECT_FALSE(
      cache.can_accept(Uniquifier{.block_expiry_height = 11, .nonce = 11}));
  EXPECT_TRUE(
      cache.can_accept(Uniquifier{.block_expiry_height = 13, .nonce = 13}));
  EXPECT_FALSE(
      cache.can_accept(Uniquifier{.block_expiry_height = 14, .nonce = 14}));
  EXPECT_TRUE(
      cache.can_accept(Uniquifier{.block_expiry_height = 15, .nonce = 15}));

  EXPECT_FALSE(
      cache.contains(Uniquifier{.block_expiry_height = 10, .nonce = 10}));
  EXPECT_TRUE(
      cache.contains(Uniquifier{.block_expiry_height = 11, .nonce = 11}));
  EXPECT_TRUE(
      cache.contains(Uniquifier{.block_expiry_height = 12, .nonce = 12}));
  EXPECT_FALSE(
      cache.contains(Uniquifier{.block_expiry_height = 13, .nonce = 13}));
  EXPECT_TRUE(
      cache.contains(Uniquifier{.block_expiry_height = 14, .nonce = 14}));
  EXPECT_FALSE(
      cache.contains(Uniquifier{.block_expiry_height = 15, .nonce = 15}));

  EXPECT_EQ(cache.uniquifiers().size(), 3);
}

TEST(ReplayCache, CountRemainingAfterClear)
{
  ReplayCache cache({Uniquifier{.block_expiry_height = 11, .nonce = 11},
                     Uniquifier{.block_expiry_height = 12, .nonce = 12},
                     Uniquifier{.block_expiry_height = 14, .nonce = 14},
                     Uniquifier{.block_expiry_height = 15, .nonce = 15}},
                    10);

  ReplayCache cache2(cache.uniquifiers(), 12);

  EXPECT_EQ(cache2.committed_size(), 3);
}

} // namespace hogd
