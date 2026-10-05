#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "hogd/base/rc.h"

namespace hogd {

namespace {

// Live Tracked instances, and copies made, to check when Rc frees a value and
// that it never copies one.
int live = 0;
int copies = 0;

struct Tracked {
  explicit Tracked(int v) : value(v) { ++live; }
  Tracked(Tracked const &other) : value(other.value)
  {
    ++live;
    ++copies;
  }
  Tracked &operator=(Tracked const &) = delete;
  ~Tracked() { --live; }

  int value;
};

class RcTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    live = 0;
    copies = 0;
  }
  void TearDown() override
  {
    EXPECT_EQ(live, 0);
    EXPECT_EQ(copies, 0);
  }
};

} // namespace

TEST_F(RcTest, CopiesShareTheValue)
{
  auto a = make_rc<Tracked>(7);
  EXPECT_EQ(a.use_count(), 1U);

  Rc<Tracked> b = a; // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(a.use_count(), 2U);
  EXPECT_EQ(live, 1);

  // A change through one copy is seen through the other.
  b->value = 8;
  EXPECT_EQ(a->value, 8);
  (*a).value = 9;
  EXPECT_EQ(b->value, 9);
}

TEST_F(RcTest, ConstValue)
{
  auto a = make_rc<const Tracked>(4);
  static_assert(std::is_same_v<decltype(*a), Tracked const &>);
  static_assert(std::is_same_v<decltype(a.get()), Tracked const *>);

  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  Rc<const Tracked> b = a;
  EXPECT_EQ(b->value, 4);
  EXPECT_EQ(a.use_count(), 2U);
}

TEST_F(RcTest, EmptyByDefault)
{
  Rc<Tracked> a;
  EXPECT_FALSE(a);
  EXPECT_EQ(a.get(), nullptr);
  EXPECT_EQ(a.use_count(), 0U);

  Rc<Tracked> b = a; // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_FALSE(b);

  a = make_rc<Tracked>(1);
  EXPECT_TRUE(a);
}

TEST_F(RcTest, LastReleaseFreesTheValue)
{
  {
    auto a = make_rc<Tracked>(1);
    {
      Rc<Tracked> b = a; // NOLINT(performance-unnecessary-copy-initialization)
      EXPECT_EQ(live, 1);
    }
    EXPECT_EQ(a.use_count(), 1U);
    EXPECT_EQ(live, 1);
  }
  EXPECT_EQ(live, 0);
}

TEST_F(RcTest, MoveTransfersWithoutCounting)
{
  auto a = make_rc<Tracked>(3);
  Tracked const *value = a.get();

  Rc<Tracked> b = std::move(a);
  EXPECT_EQ(b.get(), value);
  EXPECT_EQ(b.use_count(), 1U);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(a);

  a = b;
  EXPECT_EQ(a.get(), value);
  EXPECT_EQ(b.use_count(), 2U);
}

TEST_F(RcTest, AssignmentReleasesTheOldValue)
{
  auto a = make_rc<Tracked>(1);
  auto b = make_rc<Tracked>(2);
  EXPECT_EQ(live, 2);

  a = b;
  EXPECT_EQ(live, 1);
  EXPECT_EQ(a->value, 2);
  EXPECT_EQ(b.use_count(), 2U);

  a = make_rc<Tracked>(3);
  EXPECT_EQ(live, 2);
  EXPECT_EQ(a->value, 3);
  EXPECT_EQ(b.use_count(), 1U);

  a = Rc<Tracked>();
  EXPECT_EQ(live, 1);
}

TEST_F(RcTest, SelfAssignmentKeepsTheValue)
{
  auto a = make_rc<Tracked>(5);
  Rc<Tracked> &alias = a;
  a = alias;
  EXPECT_EQ(a->value, 5);
  EXPECT_EQ(a.use_count(), 1U);

  a = std::move(alias);
  EXPECT_EQ(a->value, 5);
  EXPECT_EQ(a.use_count(), 1U);
}

TEST_F(RcTest, ConstructsInPlace)
{
  auto v = make_rc<std::vector<std::string>>(3, "x");
  EXPECT_EQ(v->size(), 3U);
  EXPECT_EQ((*v)[2], "x");

  auto empty = make_rc<std::string>();
  EXPECT_TRUE(empty->empty());
}

} // namespace hogd
