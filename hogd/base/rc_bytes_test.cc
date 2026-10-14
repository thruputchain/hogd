#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/rc_bytes.h"

namespace hogd {

namespace {

bool
equals(RcBytes const &rc, std::vector<uint8_t> const &bytes)
{
  return std::ranges::equal(rc.span(), bytes);
}

} // namespace

TEST(RcBytesTest, EmptyByDefault)
{
  RcBytes a;
  EXPECT_TRUE(a.span().empty());
  EXPECT_EQ(a.size(), 0U);
  EXPECT_EQ(a.use_count(), 0U);

  RcBytes b = a; // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(b.use_count(), 0U);
}

TEST(RcBytesTest, CopiesTheInput)
{
  std::vector<uint8_t> input{1, 2, 3};
  RcBytes a(input);
  EXPECT_EQ(a.size(), 3U);
  EXPECT_EQ(a.use_count(), 1U);
  EXPECT_NE(a.span().data(), input.data());

  input[0] = 9;
  EXPECT_TRUE(equals(a, {1, 2, 3}));
}

TEST(RcBytesTest, EmptyInput)
{
  RcBytes a{std::span<const uint8_t>{}};
  EXPECT_TRUE(a.span().empty());
  EXPECT_EQ(a.use_count(), 1U);
}

TEST(RcBytesTest, CopiesShareTheBytes)
{
  RcBytes a(std::vector<uint8_t>(4096, 0x5a));
  RcBytes b = a; // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(a.span().data(), b.span().data());
  EXPECT_EQ(a.use_count(), 2U);
  {
    RcBytes c = b; // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(a.use_count(), 3U);
  }
  EXPECT_EQ(a.use_count(), 2U);
  EXPECT_TRUE(equals(b, std::vector<uint8_t>(4096, 0x5a)));
}

TEST(RcBytesTest, MoveTransfersWithoutCounting)
{
  RcBytes a(std::vector<uint8_t>{7, 8});
  uint8_t const *data = a.span().data();

  RcBytes b = std::move(a);
  EXPECT_EQ(b.span().data(), data);
  EXPECT_EQ(b.use_count(), 1U);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(a.use_count(), 0U);

  a = b;
  EXPECT_EQ(a.span().data(), data);
  EXPECT_EQ(b.use_count(), 2U);
}

TEST(RcBytesTest, AssignmentReleasesTheOldBytes)
{
  RcBytes a(std::vector<uint8_t>{1});
  RcBytes b(std::vector<uint8_t>{2});
  RcBytes keep = a; // NOLINT(performance-unnecessary-copy-initialization)

  a = b;
  EXPECT_TRUE(equals(a, {2}));
  EXPECT_EQ(b.use_count(), 2U);
  EXPECT_EQ(keep.use_count(), 1U);

  a = RcBytes();
  EXPECT_EQ(a.use_count(), 0U);
  EXPECT_EQ(b.use_count(), 1U);
}

TEST(RcBytesTest, SelfAssignmentKeepsTheBytes)
{
  RcBytes a(std::vector<uint8_t>{5, 6});
  RcBytes &alias = a;
  a = alias;
  EXPECT_TRUE(equals(a, {5, 6}));
  EXPECT_EQ(a.use_count(), 1U);

  a = std::move(alias);
  EXPECT_TRUE(equals(a, {5, 6}));
  EXPECT_EQ(a.use_count(), 1U);
}

} // namespace hogd
