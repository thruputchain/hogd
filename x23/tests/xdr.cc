#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <ostream>
#include <random>

#include <utility>
#include <x23/async.h>
#include <x23/format.h>
#include <x23/serialize.h>
#include <x23/vec.h>
#include <x23/x23.h>

#include "tests/xdrtest.hh"

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

struct BoxTree {
  int32_t value;
  x23::Ptr<BoxTree> left;
  x23::Ptr<BoxTree> right;

  bool operator==(const BoxTree &) const noexcept = default;
};

template<size_t I, x23::references<BoxTree> T> requires (I < 3)
decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).value);
  if constexpr (I == 1)
    return (std::forward<T>(t).left);
  if constexpr (I == 2)
    return (std::forward<T>(t).right);
}

template<>
struct std::tuple_size<BoxTree> : std::integral_constant<std::size_t, 3> {};
template<size_t I> requires (I < 3)
struct std::tuple_element<I, BoxTree> {
  using type = decltype(auto(get<I>(std::declval<BoxTree>())));
};

template<> struct x23::xdr_meta<BoxTree> {
  static constexpr XdrCategory category = XdrCategory::kStruct;
  static constexpr const char *type_name = "BoxTree";
  static constexpr std::array<std::string_view, 3> field_names{"value", "left",
                                                               "right"};
};

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

  MutualFrame mf1, mf2;
  EXPECT_EQ(mf1, mf2);

  mf1.body.which(0);
  mf2.body.which(0);
  mf1.body.success().x = 1;
  mf2.body.success().x = 2;
  EXPECT_LT(mf1, mf2);
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

namespace {

StorageDelta
make_nnint64_set_add(int64_t set, int64_t add)
{
  StorageDelta out;
  out.type(StorageDeltaType::kNonnegativeInt64SetAdd);
  out.nnint64() =
      NonnegativeInt64SetAddArgs{.set_value = set, .add_value = add};
  return out;
}

} // namespace

TEST(UnionAssign, BasicAssertion)
{
  StorageDeltaVec dv;

  dv.emplace_back();

  StorageDelta d = make_nnint64_set_add(100, 10);

  dv.back() = d;

  EXPECT_EQ(d.type(), dv.back().type());
}

TEST(CoReturnCompile, UniquePtr)
{
  auto x = [](int x) -> x23::Async<std::unique_ptr<int>> {
    auto out = std::make_unique<int>(x);
    co_return out;
  };

  auto res = x(10)();
  ASSERT_EQ(*res, 10);
}

TEST(CommentParsing, MultilineComments)
{
  // Check that multiline comments are parsed correctly
  [[maybe_unused]]
  StellarValue sv;
  sv.value = 100;

  [[maybe_unused]]
  StellarValueEnum sv_enum = StellarValueEnum::kStellarValue;
}

TEST(ExtensionPoint, CompileCheck)
{
  ParseFailure pf{};

  x23::Buf buf = x23::serialize(pf);

  try {
    deserialize(&pf, x23::BufViewEnd(buf));
  } catch (const x23::XdrException &e) {
    GTEST_FAIL() << "parse failure";
  }
}

TEST(ExtensionPoint, Constexpr)
{
  constexpr ExtensionPoint ep(0);
  static_assert(ep.v() == 0);
  constexpr ExtensionPoint ep_default{};
  static_assert(ep_default.v() == 0);
  constexpr ParseFailure pf{.ext = ExtensionPoint(0), .value = 1};
  static_assert(pf.ext.v() == 0);
  static_assert(pf.value == 1);
}

TEST(XdrPtr, Constructor)
{
  {
    x23::XdrVec<x23::Ptr<int32_t>> v;
    v.emplace_back(nullptr);
    v.emplace_back(0);
    EXPECT_FALSE(v[0]);
    EXPECT_TRUE(v[1]);
  }

  {
    x23::Ptr<x23::XdrVec<int32_t>> v{std::in_place, {0}};
    EXPECT_EQ(v.size(), 1UZ);
  }

  {
    x23::Ptr<fix_12> p{{.i = 1, .d = 2.0}};
    x23::Ptr p2{fix_12{.i = 1, .d = 2.0}};
    EXPECT_EQ(p, p2);
    x23::XdrVec<x23::Ptr<fix_12>> v{fix_12{.i = 1, .d = 2.0}, nullptr,
                                    fix_12{.i = 4, .d = 8.0}};
    EXPECT_EQ(v.size(), 3UZ);
  }
}

TEST(XdrPtr, ThreeWayCompareUsesPointeeValue)
{
  x23::Ptr<int32_t> a(7);
  x23::Ptr<int32_t> b(7);
  x23::Ptr<int32_t> c(9);

  EXPECT_EQ(a, b);
  EXPECT_TRUE((a <=> b) == 0);
  EXPECT_LT(a, c);
  EXPECT_TRUE((a <=> c) < 0);
}

TEST(Box, RecursiveRoundTrip)
{
  BoxTree tree{
      .value = 1,
      .left = BoxTree{.value = 2},
      .right = BoxTree{.value = 3, .left = BoxTree{.value = 4}},
  };

  x23::Box<BoxTree> boxed(tree);
  auto buf = x23::serialize(boxed);

  auto plain_buf = x23::serialize(tree);
  EXPECT_EQ(buf, plain_buf);

  x23::Box<BoxTree> result;
  x23::deserialize(&result, buf);
  EXPECT_EQ(*result, tree);
}

TEST(Box, GeneratedBoxExpr)
{
  using enum box_expr_type;

  // Build expression tree: (1 + 2)
  box_expr one(kBoxLiteral);
  one.literal() = 1;
  box_expr two(kBoxLiteral);
  two.literal() = 2;

  box_add_args args;
  args.left = one;
  args.right = two;

  box_expr add(kBoxAdd);
  add.add() = x23::Box<box_add_args>(std::move(args));

  auto buf = x23::serialize(add);
  box_expr result;
  x23::deserialize(&result, buf);
  EXPECT_EQ(result, add);
  EXPECT_EQ((*result.add())->left.literal(), 1);
  EXPECT_EQ((*result.add())->right.literal(), 2);
}
