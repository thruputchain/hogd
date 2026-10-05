#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "hogd/base/rc.h"

namespace hogd {

namespace {

// Live Tracked instances, and copies made, to check when Rc copies or frees a
// value.
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
  void TearDown() override { EXPECT_EQ(live, 0); }
};

} // namespace

TEST_F(RcTest, CopiesShareTheValue)
{
  Rc<Tracked> a(std::in_place, 7);
  EXPECT_EQ(a.use_count(), 1U);

  Rc<Tracked> b = a; // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(&*a, &*b);
  EXPECT_EQ(b->value, 7);
  EXPECT_EQ(a.use_count(), 2U);
  EXPECT_EQ(live, 1);
  EXPECT_EQ(copies, 0);
}

TEST_F(RcTest, LastReleaseFreesTheValue)
{
  {
    Rc<Tracked> a(std::in_place, 1);
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
  Rc<Tracked> a(std::in_place, 3);
  Tracked const *value = &*a;

  Rc<Tracked> b = std::move(a);
  EXPECT_EQ(&*b, value);
  EXPECT_EQ(b.use_count(), 1U);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(a.use_count(), 0U);

  // A moved-from Rc can be assigned to again.
  a = b;
  EXPECT_EQ(&*a, value);
  EXPECT_EQ(b.use_count(), 2U);
}

TEST_F(RcTest, AssignmentReleasesTheOldValue)
{
  Rc<Tracked> a(std::in_place, 1);
  Rc<Tracked> b(std::in_place, 2);
  EXPECT_EQ(live, 2);

  a = b;
  EXPECT_EQ(live, 1);
  EXPECT_EQ(a->value, 2);
  EXPECT_EQ(b.use_count(), 2U);

  a = Rc<Tracked>(std::in_place, 3);
  EXPECT_EQ(live, 2);
  EXPECT_EQ(a->value, 3);
  EXPECT_EQ(b.use_count(), 1U);
}

TEST_F(RcTest, SelfAssignmentKeepsTheValue)
{
  Rc<Tracked> a(std::in_place, 5);
  Rc<Tracked> &alias = a;
  a = alias;
  EXPECT_EQ(a->value, 5);
  EXPECT_EQ(a.use_count(), 1U);

  a = std::move(alias);
  EXPECT_EQ(a->value, 5);
  EXPECT_EQ(a.use_count(), 1U);
}

TEST_F(RcTest, MakeMutOnAnUnsharedValueDoesNotCopy)
{
  Rc<Tracked> a(std::in_place, 1);
  Tracked const *value = &*a;
  a.make_mut().value = 2;
  EXPECT_EQ(&*a, value);
  EXPECT_EQ(a->value, 2);
  EXPECT_EQ(copies, 0);
}

TEST_F(RcTest, MakeMutOnASharedValueCopiesFirst)
{
  Rc<Tracked> a(std::in_place, 1);
  Rc<Tracked> b = a;
  Rc<Tracked> c = a; // NOLINT(performance-unnecessary-copy-initialization)

  b.make_mut().value = 2;
  EXPECT_EQ(copies, 1);
  EXPECT_NE(&*a, &*b);
  EXPECT_EQ(a->value, 1);
  EXPECT_EQ(b->value, 2);
  EXPECT_EQ(&*a, &*c);
  EXPECT_EQ(a.use_count(), 2U);
  EXPECT_EQ(b.use_count(), 1U);

  // b's value is now its own.
  b.make_mut().value = 3;
  EXPECT_EQ(copies, 1);
  EXPECT_EQ(a->value, 1);
}

TEST_F(RcTest, ConstructsInPlace)
{
  Rc<std::vector<std::string>> v(std::in_place, 3, "x");
  EXPECT_EQ(v->size(), 3U);
  EXPECT_EQ((*v)[2], "x");

  Rc<std::string> empty(std::in_place);
  EXPECT_TRUE(empty->empty());
}

} // namespace hogd
