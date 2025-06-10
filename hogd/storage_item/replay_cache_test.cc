#include <gtest/gtest.h>

#include "hogd/storage_item/replay_cache.h"
#include "storage_item.hh"

namespace hogd {

TEST(ReplayCache, Basic)
{
  ReplayCache cache(ReplayCacheMetadata{.minimum_threshold = 10}, {11, 12, 14});

  EXPECT_FALSE(cache.can_accept(9));
  EXPECT_FALSE(cache.can_accept(10));
  EXPECT_FALSE(cache.can_accept(11));
  EXPECT_TRUE(cache.can_accept(13));
  EXPECT_FALSE(cache.can_accept(14));
  EXPECT_TRUE(cache.can_accept(15));

  EXPECT_FALSE(cache.contains(10));
  EXPECT_TRUE(cache.contains(11));
  EXPECT_TRUE(cache.contains(12));
  EXPECT_FALSE(cache.contains(13));
  EXPECT_TRUE(cache.contains(14));
  EXPECT_FALSE(cache.contains(15));

  EXPECT_EQ(cache.size(), 3);
}

TEST(ReplayCache, CountRemainingAfterClear)
{
  ReplayCache cache(ReplayCacheMetadata{.minimum_threshold = 10},
                    {11, 12, 14, 15});

  EXPECT_EQ(cache.count_remaining_after_clear(10), 4);
  EXPECT_EQ(cache.count_remaining_after_clear(11), 3);
  EXPECT_EQ(cache.count_remaining_after_clear(12), 2);
  EXPECT_EQ(cache.count_remaining_after_clear(13), 2);
  EXPECT_EQ(cache.count_remaining_after_clear(14), 1);
  EXPECT_EQ(cache.count_remaining_after_clear(15), 0);
  EXPECT_EQ(cache.count_remaining_after_clear(16), 0);
}

} // namespace hogd
