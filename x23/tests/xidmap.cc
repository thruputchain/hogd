#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "x23/xidmap.h"

using x23::detail::XidMap;

namespace {

// GTest registers tests through static initializers.
// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(XidMap, WrapGrowthAndReverseCompletion)
{
  XidMap<std::uintptr_t> map(std::numeric_limits<std::uint32_t>::max() - 8);
  std::vector<std::uint32_t> xids;
  for (std::uintptr_t value = 1; value <= 4096; ++value)
    xids.push_back(map.insert(value));
  EXPECT_EQ(map.size(), xids.size());
  EXPECT_NE(std::ranges::find(xids, 0), xids.end());
  for (size_t i = xids.size(); i; --i) {
    EXPECT_EQ(map.take(xids[i - 1]), i);
    EXPECT_EQ(map.take(xids[i - 1]), 0U);
  }
  EXPECT_EQ(map.size(), 0U);
}

TEST(XidMap, LongLivedCallAndStaleReplies)
{
  XidMap<std::uintptr_t> map;
  const auto held = map.insert(1U);
  const auto stale = map.insert(2U);
  EXPECT_EQ(map.take(stale), 2U);
  for (unsigned i = 0; i < 10000; ++i) {
    const auto xid = map.insert(3U);
    ASSERT_NE(xid, held);
    EXPECT_EQ(map.take(stale), 0U);
    EXPECT_EQ(map.take(xid + 0x100000U), 0U);
    EXPECT_EQ(map.take(xid), 3U);
  }
  EXPECT_EQ(map.size(), 1U);
  EXPECT_EQ(map.take(held), 1U);
}

TEST(XidMap, RandomOperationsMatchReference)
{
  XidMap<std::uintptr_t> map(0xfffffff0U);
  std::map<std::uint32_t, std::uintptr_t> reference;
  std::vector<std::uint32_t> live;
  // A fixed seed makes failures reproducible.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 random(4871);
  for (std::uintptr_t value = 1; value <= 100000; ++value) {
    if (live.empty() || (live.size() < 2048 && random() % 2)) {
      auto xid = map.insert(value);
      ASSERT_TRUE(reference.emplace(xid, value).second);
      live.push_back(xid);
    }
    else {
      auto i = random() % live.size();
      auto xid = live[i];
      ASSERT_EQ(map.take(xid), reference.at(xid));
      EXPECT_EQ(map.take(xid), 0U);
      reference.erase(xid);
      live[i] = live.back();
      live.pop_back();
    }
    auto xid = random();
    if (!reference.contains(xid)) {
      EXPECT_EQ(map.take(xid), 0U);
    }
    ASSERT_EQ(map.size(), reference.size());
  }
  for (const auto &[xid, value] : reference)
    EXPECT_EQ(map.take(xid), value);
  EXPECT_EQ(map.size(), 0U);
}

TEST(XidMap, DetachBeforeFailingCallbacks)
{
  XidMap<std::uintptr_t> map;
  for (unsigned i = 1; i <= 100; ++i)
    map.insert(i);
  auto old = std::exchange(map, {});
  unsigned count = 0;
  old.for_each([&](auto value) {
    ++count;
    map.insert(value);
  });
  EXPECT_EQ(count, 100U);
  EXPECT_EQ(map.size(), 100U);
}
// NOLINTEND(bugprone-throwing-static-initialization)

} // namespace
