#include <gtest/gtest.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "hogd/base/arc.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/utility.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

// Live Tracked instances, and copies made, to check when Arc frees a value and
// that sharing one never copies it.
int live = 0;
int copies = 0;

struct Tracked {
  Tracked() { ++live; }
  explicit Tracked(int v) : value(v) { ++live; }
  Tracked(Tracked const &other) : value(other.value)
  {
    ++live;
    ++copies;
  }
  Tracked(Tracked &&other) noexcept : value(other.value) { ++live; }
  Tracked &operator=(Tracked const &) = delete;
  Tracked &operator=(Tracked &&) = delete;
  ~Tracked() { --live; }

  int value = 0;
};

// For reads of an empty Arc, which touch a static T{} that lives on past the
// test and so cannot be a Tracked.
struct Plain {
  int value = 0;
};

class ArcTest : public ::testing::Test {
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

// A small XDR struct, with the tuple protocol x23c would generate for it, to
// check that x23 reads and writes an Arc of it as the struct itself.
struct ArcTestPoint {
  int32_t x = 0;
  x23::Vec<uint8_t, 16> payload;

  bool operator==(const ArcTestPoint &) const noexcept = default;
  auto operator<=>(const ArcTestPoint &) const noexcept = default;
};

template<size_t I, x23::references<ArcTestPoint> T> requires (I < 2)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).x);
  if constexpr (I == 1)
    return (std::forward<T>(t).payload);
}

} // namespace

} // namespace hogd

template<>
struct std::tuple_size<hogd::ArcTestPoint>
  : std::integral_constant<std::size_t, 2> {};
template<std::size_t I> requires (I < 2)
struct std::tuple_element<I, hogd::ArcTestPoint> {
  using type = decltype(auto(get<I>(hogd::ArcTestPoint{})));
};
// NOLINTBEGIN(readability-identifier-naming)
template<> struct x23::xdr_meta<hogd::ArcTestPoint> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "ArcTestPoint";
  // Part of the struct protocol, though nothing in this test reads it.
  [[maybe_unused]] static constexpr std::array<std::string_view, 2> field_names{
      "x", "payload"};
};
// NOLINTEND(readability-identifier-naming)

namespace hogd {

TEST_F(ArcTest, CopiesShareTheValue)
{
  auto a = make_arc<Tracked>(7);
  EXPECT_EQ(a.use_count(), 1U);

  Arc<Tracked> b = a; // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(a.use_count(), 2U);
  EXPECT_EQ(live, 1);

  // A change through one copy is seen through the other: shared_ptr
  // semantics, not copy-on-write.
  b->value = 8;
  EXPECT_EQ(a->value, 8);
  (*a).value = 9;
  EXPECT_EQ(b->value, 9);
}

TEST_F(ArcTest, EmptyReadsAsAValueInitializedValue)
{
  Arc<Plain> a;
  EXPECT_FALSE(a.has_value());
  EXPECT_EQ(a.use_count(), 0U);

  // Reading through a const Arc never allocates: every empty Arc reads the
  // same static value.
  EXPECT_EQ(std::as_const(a)->value, 0);
  EXPECT_EQ((*std::as_const(a)).value, 0);
  EXPECT_EQ(std::as_const(a).get(), &Arc<Plain>::empty_value());
  EXPECT_FALSE(a.has_value());

  // Writing through an empty Arc allocates its value first.
  a->value = 1;
  EXPECT_TRUE(a.has_value());
  EXPECT_EQ(a.use_count(), 1U);
  EXPECT_EQ(std::as_const(a)->value, 1);
}

TEST_F(ArcTest, LastReleaseFreesTheValue)
{
  {
    auto a = make_arc<Tracked>(1);
    {
      Arc<Tracked> b = a; // NOLINT(performance-unnecessary-copy-initialization)
      EXPECT_EQ(live, 1);
    }
    EXPECT_EQ(a.use_count(), 1U);
    EXPECT_EQ(live, 1);
  }
  EXPECT_EQ(live, 0);
}

TEST_F(ArcTest, MoveTransfersWithoutCounting)
{
  auto a = make_arc<Tracked>(3);
  Tracked const *value = a.get();

  Arc<Tracked> b = std::move(a);
  EXPECT_EQ(b.get(), value);
  EXPECT_EQ(b.use_count(), 1U);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(a.has_value());

  a = b;
  EXPECT_EQ(a.get(), value);
  EXPECT_EQ(b.use_count(), 2U);
}

TEST_F(ArcTest, WrapsAValueWithoutCopying)
{
  Arc<Tracked> a = Tracked(4);
  EXPECT_TRUE(a.has_value());
  EXPECT_EQ(a->value, 4);
  EXPECT_EQ(live, 1);
}

TEST_F(ArcTest, SharesAsAStdSharedPtr)
{
  auto a = make_arc<Tracked>(5);
  std::shared_ptr<Tracked> p = a.share();
  EXPECT_EQ(p.get(), a.get());
  EXPECT_EQ(a.use_count(), 2U);

  std::shared_ptr<const Tracked> q = std::as_const(a).share();
  EXPECT_EQ(q.get(), a.get());
  EXPECT_EQ(a.use_count(), 3U);

  // An empty Arc shares its value-initialized value without allocating.
  Arc<Plain> empty;
  std::shared_ptr<const Plain> e = std::as_const(empty).share();
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->value, 0);
  EXPECT_FALSE(empty.has_value());
}

TEST_F(ArcTest, ComparesByValue)
{
  Arc<ArcTestPoint> a = ArcTestPoint{.x = 1, .payload = {1, 2}};
  Arc<ArcTestPoint> b = ArcTestPoint{.x = 1, .payload = {1, 2}};
  Arc<ArcTestPoint> c = ArcTestPoint{.x = 2, .payload = {1, 2}};
  EXPECT_NE(a.get(), b.get());
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_LT(a, c);

  Arc<ArcTestPoint> empty;
  EXPECT_EQ(empty, Arc<ArcTestPoint>(ArcTestPoint{}));
  EXPECT_EQ(empty, Arc<ArcTestPoint>());
}

TEST_F(ArcTest, ComparesSharedValuesByValueToo)
{
  // Sharing must not change the answer: a NaN is not equal to itself, even
  // through two Arcs that hold the same one.
  auto a = make_arc<double>(std::numeric_limits<double>::quiet_NaN());
  Arc<double> b = a; // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_FALSE(a == b);
  EXPECT_TRUE((a <=> b) == std::partial_ordering::unordered);
}

TEST_F(ArcTest, UnpacksWithStructuredBindings)
{
  Arc<ArcTestPoint> a = ArcTestPoint{.x = 3, .payload = {1, 2}};
  auto [x, payload] = a;
  EXPECT_EQ(x, 3);
  EXPECT_EQ(payload.size(), 2U);

  auto &[rx, rpayload] = a;
  rx = 4;
  EXPECT_EQ(a->x, 4);

  const Arc<ArcTestPoint> &c = a;
  const auto &[cx, cpayload] = c;
  EXPECT_EQ(cx, 4);
}

TEST(ArcXdr, HasTheWireFormatOfItsValue)
{
  const ArcTestPoint point{.x = 42, .payload = {0xaa, 0xbb, 0xcc}};
  const x23::Buf bytes = x23::serialize(point);

  Arc<ArcTestPoint> shared = point;
  EXPECT_EQ(x23::serialize(shared), bytes);

  // An empty Arc writes a value-initialized value.
  EXPECT_EQ(x23::serialize(Arc<ArcTestPoint>()),
            x23::serialize(ArcTestPoint{}));
}

TEST(ArcXdr, DecodesStraightIntoAnEmptyArc)
{
  const ArcTestPoint point{.x = 42, .payload = {0xaa, 0xbb, 0xcc}};
  const x23::Buf bytes = x23::serialize(point);

  Arc<ArcTestPoint> decoded;
  x23::deserialize(&decoded, bytes);
  EXPECT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, point);
}

TEST(ArcXdr, DecodesAVectorIntoDistinctValues)
{
  const x23::Vec<ArcTestPoint> plain{
      ArcTestPoint{.x = 1, .payload = {1}},
      ArcTestPoint{.x = 2, .payload = {2, 2}},
      ArcTestPoint{.x = 3, .payload = {}},
  };
  const x23::Buf bytes = x23::serialize(plain);

  x23::Vec<Arc<ArcTestPoint>> shared;
  x23::deserialize(&shared, bytes);
  ASSERT_EQ(shared.size(), plain.size());
  for (size_t i = 0; i < plain.size(); i++) {
    EXPECT_TRUE(shared[i].has_value());
    EXPECT_EQ(*shared[i], plain[i]);
  }
  EXPECT_NE(shared[0].get(), shared[1].get());
  EXPECT_NE(shared[1].get(), shared[2].get());

  EXPECT_EQ(x23::serialize(shared), bytes);
}

TEST(ArcXdr, DecodesAsAStructField)
{
  const std::tuple<ArcTestPoint, uint32_t> plain{
      ArcTestPoint{.x = 9, .payload = {9, 9, 9, 9, 9}}, 7};
  const x23::Buf bytes = x23::serialize(plain);

  std::tuple<Arc<ArcTestPoint>, uint32_t> shared;
  x23::deserialize(&shared, bytes);
  EXPECT_TRUE(std::get<0>(shared).has_value());
  EXPECT_EQ(*std::get<0>(shared), std::get<0>(plain));
  EXPECT_EQ(std::get<1>(shared), 7U);

  EXPECT_EQ(x23::serialize(shared), bytes);
}

TEST(ArcXdr, DecodingIntoASharedArcWritesTheSharedValue)
{
  const ArcTestPoint point{.x = 5, .payload = {5}};
  const x23::Buf bytes = x23::serialize(point);

  Arc<ArcTestPoint> a = ArcTestPoint{};
  Arc<ArcTestPoint> b =
      a; // NOLINT(performance-unnecessary-copy-initialization)
  x23::deserialize(&a, bytes);
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(*b, point);
}

TEST(ArcXdr, ResizingWithAValueGivesIndependentElements)
{
  // A Vec resized from one value copies that value into every slot. Empty
  // Arcs copy as empty handles, so each element allocates its own value when
  // it is first written, as a batch decode does.
  x23::Vec<Arc<ArcTestPoint>> v;
  v.resize(3, {});
  get<0>(v[0]) = 1;
  EXPECT_TRUE(v[0].has_value());
  EXPECT_FALSE(v[1].has_value());
  EXPECT_FALSE(v[2].has_value());
  EXPECT_EQ(std::as_const(v[1])->x, 0);
  EXPECT_NE(v[0].get(), std::as_const(v[1]).get());
}

TEST(ArcXdr, StandsInForItsValueWhereAnRpcExpectsIt)
{
  static_assert(x23::serializes_like<Arc<ArcTestPoint>, ArcTestPoint>);
  static_assert(x23::serializes_like<x23::Vec<Arc<ArcTestPoint>>,
                                     x23::Vec<ArcTestPoint>>);
  static_assert(x23::serializes_like<x23::span<const Arc<ArcTestPoint>>,
                                     x23::Vec<ArcTestPoint>>);
  // An Arc of anything x23 cannot read or write through it must fail the type
  // check too, rather than pass it and fail when written.
  static_assert(!x23::serializes_like<Arc<int32_t>, int32_t>);

  const x23::Vec<ArcTestPoint> plain{
      ArcTestPoint{.x = 1, .payload = {1}},
      ArcTestPoint{.x = 2, .payload = {2, 2}},
  };
  x23::Vec<Arc<ArcTestPoint>> shared{plain[0], plain[1]};
  x23::span<const Arc<ArcTestPoint>> view(shared);
  EXPECT_EQ(x23::serialize(view), x23::serialize(plain));
}

// The union tests use a generated union, since the union protocol is too
// involved to write by hand.
TEST(ArcXdr, DecodesAUnion)
{
  static_assert(x23::is_xdr_union<Arc<Signature>>);

  Signature sig(CryptographicScheme::Ed25519);
  sig.ed25519().fill(0x5a);
  const x23::Buf bytes = x23::serialize(sig);

  Arc<Signature> decoded;
  x23::deserialize(&decoded, bytes);
  EXPECT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, sig);
  EXPECT_EQ(decoded->scheme(), CryptographicScheme::Ed25519);
  EXPECT_EQ(get<0>(decoded), CryptographicScheme::Ed25519);
  EXPECT_EQ(x23::serialize(decoded), bytes);
}

TEST(ArcXdr, DecodesAVectorOfUnionsWithEveryArm)
{
  x23::Vec<Signature> plain(2);
  plain[0] = Signature(CryptographicScheme::Ed25519);
  plain[0].ed25519().fill(0x11);
  plain[1] = Signature(CryptographicScheme::MlDsa65);
  plain[1].mldsa65()->fill(0x22);
  const x23::Buf bytes = x23::serialize(plain);

  x23::Vec<Arc<Signature>> shared;
  x23::deserialize(&shared, bytes);
  ASSERT_EQ(shared.size(), 2U);
  EXPECT_EQ(*shared[0], plain[0]);
  EXPECT_EQ(*shared[1], plain[1]);
  EXPECT_EQ(shared[1]->scheme(), CryptographicScheme::MlDsa65);
  EXPECT_EQ(x23::serialize(shared), bytes);
}

TEST(ArcXdr, SettingAUnionDiscriminantAllocatesAnEmptyArc)
{
  Arc<Signature> a;
  EXPECT_EQ(std::as_const(a)->scheme(), CryptographicScheme::Ed25519);
  EXPECT_FALSE(a.has_value());

  x23::union_set_discriminant(a, CryptographicScheme::MlDsa65);
  EXPECT_TRUE(a.has_value());
  EXPECT_EQ(a->scheme(), CryptographicScheme::MlDsa65);
  EXPECT_TRUE(x23::union_valid(a));
}

} // namespace hogd
