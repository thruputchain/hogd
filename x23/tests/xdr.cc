
#include <gtest/gtest.h>
#include <x23/format.h>

#include "tests/xdrtest.hh"
#include "x23/serialize.h"

// This causes googletest to pretty-print XDR structures
void
PrintTo(const x23::is_xdr_type auto &obj, std::ostream *os)
{
  *os << x23::show(obj);
}

TEST(CompareStructs, BasicAssertions)
{
  EXPECT_EQ((fix_12{.i = 0, .d = 100.0}), (fix_12{.i = 0, .d = 100.0}));
  EXPECT_LE((fix_12{.i = 0, .d = 100.0}), (fix_12{.i = 0, .d = 100.0}));
  EXPECT_NE((fix_12{.i = 0, .d = 100.0}), (fix_12{.i = 1, .d = 0.0}));
  EXPECT_LT((fix_12{.i = 0, .d = 100.0}), (fix_12{.i = 1, .d = 0.0}));
  EXPECT_GT((fix_12{.i = 1, .d = 100.0}), (fix_12{.i = 1, .d = 0.0}));

  uunion ua(1);
  uunion ub(1);
  ub.one() = true;
  EXPECT_LT(ua, ub);
  ua.d(2);
  ua.two() = -100;
  EXPECT_GT(ua, ub);

  ua.d(4);
  ua.four() = "AAA";
  ub.d(4);
  ub.four() = "AAAA";
  EXPECT_LE(ua, ub);

  test_recursive r1, r2;
  EXPECT_EQ(r1, r2);
  r2.next.emplace();
  EXPECT_LT(r1, r2);
}

TEST(RoundTrip, BasicAssertions)
{
  uptr ua(true);
  ua.val().emplace(6);
  uptr ub(false);
  EXPECT_NE(ua, ub);
  EXPECT_GT(ua, ub);

  auto buf = x23::serialize(ua);
  deserialize(&ub, buf);
  EXPECT_EQ(ua, ub);
}
