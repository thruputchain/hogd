
#include <random>

#include <gtest/gtest.h>
#include <x23/format.h>

#include "tests/xdrtest.hh"
#include "x23/serialize.h"

std::default_random_engine rnd;

// This causes googletest to pretty-print XDR structures
void
PrintTo(const x23::is_xdr_type auto &obj, std::ostream *os)
{
  *os << x23::show(obj);
}

x23::Buf
random_buf()
{
  std::uniform_int_distribution<std::uint8_t> gen;
  x23::Buf ret(size_t(gen(rnd)));
  for (auto &b : ret)
    b = gen(rnd);
  return ret;
}

TEST(Deserialize, Fuzz)
{
  for (int i = 0; i < 10'000; ++i) {
    x23::Buf input = random_buf();
    input.resize(input.size() - input.size() % 4);
    test_recursive r;
    try {
      deserialize_from(&r, input);
    } catch (const x23::XdrException &) {
    }
    x23::Str s;
    try {
      deserialize_from(&s, input);
    } catch (const x23::XdrException &) {
    }
    uunion uu;
    try {
      deserialize_from(&uu, input);
    } catch (const x23::XdrException &) {
    }
  }
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

StorageDelta
make_nnint64_set_add(int64_t set, int64_t add)
{
  StorageDelta out;
  out.type(StorageDeltaType::kNonnegativeInt64SetAdd);
  out.nnint64() = NonnegativeInt64SetAddArgs{set, add};
  return out;
}

TEST(UnionAssign, BasicAssertion)
{
  StorageDeltaVec dv;

  dv.emplace_back();

  StorageDelta d = make_nnint64_set_add(100, 10);

  dv.back() = d;

  EXPECT_EQ(d.type(), dv.back().type());
}
