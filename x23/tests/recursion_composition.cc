#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "tests/xdrtest.hh"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace x23 {
namespace {

enum class DecodePath { kXdr, kXdrFrom, kCxs, kCxsFrom };

test_recursive
make_chain(size_t depth)
{
  test_recursive root;
  auto *node = &root;
  for (size_t i = 1; i < depth; ++i) {
    node->next.emplace();
    node = &*node->next;
  }
  return root;
}

// Small, bounded recursive fixtures exercise sibling restoration.
// NOLINTNEXTLINE(misc-no-recursion)
test_recursive
make_tree(size_t depth, size_t width)
{
  test_recursive root;
  if (depth > 1)
    for (size_t i = 0; i < width; ++i)
      root.nextvec.emplace_back(make_tree(depth - 1, width));
  return root;
}

// Each active entry into an is_recursive_xdr struct/union consumes one level;
// returning releases that level. Non-recursive envelopes consume none. This
// preserves the current classification of recursive types, including every
// struct/union participating in a mutual-recursion cycle.
class RecursionComposition : public testing::TestWithParam<DecodePath> {
protected:
  static bool cxs()
  {
    return GetParam() == DecodePath::kCxs || GetParam() == DecodePath::kCxsFrom;
  }

  template<typename T> Buf encode(const T &value) const
  {
    if (!cxs())
      return serialize(value);
    auto bytes = serialize_cxs(value);
    EXPECT_TRUE(bytes);
    return bytes ? std::move(*bytes) : Buf{};
  }

  template<typename T>
  std::optional<size_t> decode(T &out, std::span<const uint8_t> bytes,
                               size_t limit, bool prefix = false) const
  {
    if (cxs()) {
      if (prefix || GetParam() == DecodePath::kCxsFrom) {
        auto used = deserialize_cxs_from(&out, bytes, limit);
        if (used)
          return *used;
        EXPECT_EQ(used.error(), CxsErrorCode::kRecursionLimit);
      }
      else {
        auto status = deserialize_cxs(&out, bytes, limit);
        if (status)
          return bytes.size();
        EXPECT_EQ(status.error(), CxsErrorCode::kRecursionLimit);
      }
    }
    else {
      try {
        if (prefix || GetParam() == DecodePath::kXdrFrom)
          return deserialize_from(&out, BufView(bytes), limit);
        deserialize(&out, BufView(bytes), limit);
        return bytes.size();
      } catch (const XdrBadInput &e) {
        EXPECT_TRUE(std::string_view(e.what()).starts_with(
            "x23::Deserializer: exceeded recursion limit"));
      }
    }
    return std::nullopt;
  }

  template<typename T> void expect_accepts(const T &value, size_t limit) const
  {
    auto bytes = encode(value);
    T out{};
    auto used = decode(out, bytes, limit);
    ASSERT_TRUE(used) << "limit=" << limit;
    EXPECT_EQ(*used, bytes.size());
    EXPECT_EQ(out, value);
  }

  template<typename T> void expect_rejects(const T &value, size_t limit) const
  {
    auto bytes = encode(value);
    T out{};
    EXPECT_FALSE(decode(out, bytes, limit)) << "limit=" << limit;
  }
};

// Decode the exact same wire bytes as a tuple and as two sequential values.
// Neither representation should change whether the depth bound admits them.
TEST_P(RecursionComposition, TupleMatchesSequentialDecoding)
{
  for (size_t limit : {1UZ, 4UZ, 16UZ, 64UZ}) {
    SCOPED_TRACE(limit);
    auto pair = std::tuple{make_chain(limit), make_chain(limit)};
    auto bytes = encode(pair);
    auto remaining = std::span<const uint8_t>(bytes);
    test_recursive first, second;
    auto used_first = decode(first, remaining, limit, true);
    ASSERT_TRUE(used_first);
    remaining = remaining.subspan(*used_first);
    auto used_second = decode(second, remaining, limit, true);
    ASSERT_TRUE(used_second);
    EXPECT_EQ(*used_first + *used_second, bytes.size());
    EXPECT_EQ(first, std::get<0>(pair));
    EXPECT_EQ(second, std::get<1>(pair));

    decltype(pair) together;
    auto used_together = decode(together, bytes, limit);
    ASSERT_TRUE(used_together);
    EXPECT_EQ(*used_together, bytes.size());
    EXPECT_EQ(together, pair);
  }
}

TEST_P(RecursionComposition, TupleRejectsEitherOverDepthElement)
{
  constexpr size_t kLimit = 4;
  for (bool first_is_deep : {false, true}) {
    SCOPED_TRACE(first_is_deep);
    auto first = make_chain(kLimit + (first_is_deep ? 1 : 0));
    auto second = make_chain(kLimit + (first_is_deep ? 0 : 1));
    expect_rejects(first_is_deep ? first : second, kLimit);
    expect_accepts(first_is_deep ? second : first, kLimit);
    expect_rejects(std::tuple{first, second}, kLimit);
  }
}

TEST_P(RecursionComposition, NonRecursiveEnvelopesPreserveBoundary)
{
  static_assert(!is_recursive_xdr<contains_test_recursive>);
  static_assert(!is_recursive_xdr<recursive_envelope>);
  static_assert(!is_recursive_xdr<recursive_union_envelope>);
  for (size_t limit : {1UZ, 4UZ, 16UZ}) {
    SCOPED_TRACE(limit);
    for (size_t depth : {limit, limit + 1}) {
      auto chain = make_chain(depth);
      contains_test_recursive wrapper{chain};
      recursive_envelope envelope{17, wrapper, 23};
      recursive_union_envelope union_envelope(true);
      union_envelope.payload() = envelope;
      auto tuple_envelope =
          std::tuple{uint32_t{17}, union_envelope, uint32_t{23}};
      if (depth == limit) {
        expect_accepts(chain, limit);
        expect_accepts(wrapper, limit);
        expect_accepts(envelope, limit);
        expect_accepts(union_envelope, limit);
        expect_accepts(tuple_envelope, limit);
      }
      else {
        expect_rejects(chain, limit);
        expect_rejects(wrapper, limit);
        expect_rejects(envelope, limit);
        expect_rejects(union_envelope, limit);
        expect_rejects(tuple_envelope, limit);
      }
    }
  }
}

TEST_P(RecursionComposition, ArraysAndVectorsDoNotAccumulateDepth)
{
  for (size_t limit : {1UZ, 4UZ, 16UZ}) {
    SCOPED_TRACE(limit);
    std::array<test_recursive, 3> array;
    for (auto &item : array)
      item = make_chain(limit);
    Vec<test_recursive> vector(array.begin(), array.end());
    expect_accepts(array, limit);
    expect_accepts(vector, limit);
    array.back() = make_chain(limit + 1);
    vector.front() = make_chain(limit + 1);
    expect_rejects(array, limit);
    expect_rejects(vector, limit);
  }
}

TEST_P(RecursionComposition, WidthDoesNotConsumeDepth)
{
  for (size_t width : {1UZ, 4UZ, 16UZ, 64UZ}) {
    SCOPED_TRACE(width);
    auto tree = make_tree(2, width);
    expect_accepts(tree, 2);
    expect_rejects(tree, 1);
  }
}

TEST_P(RecursionComposition, FullFourWayTreeFitsDepthFour)
{
  // Same branching and depth as a maximal auth-token tree: 85 nodes, depth 4.
  auto tree = make_tree(4, 4);
  expect_accepts(tree, 4);
  expect_rejects(tree, 3);
}

TEST_P(RecursionComposition, PointerAndVectorFieldsRestoreDepth)
{
  test_recursive root;
  root.next = Ptr<test_recursive>{new test_recursive(make_chain(3))};
  root.nextvec.emplace_back(make_chain(3));
  root.nextvec.emplace_back(make_chain(3));
  expect_accepts(root, 4);
  expect_rejects(root, 3);
}

TEST_P(RecursionComposition, LateDeepBranchStillFails)
{
  auto tree = make_tree(4, 4);
  auto *last = &tree;
  for (size_t depth = 1; depth < 4; ++depth)
    last = &last->nextvec.back();
  last->next.emplace();
  expect_rejects(tree, 4);
  expect_accepts(tree, 5);
}

TEST_P(RecursionComposition, ZeroAllowsOnlyAbsentRecursiveValues)
{
  expect_accepts(uint32_t{7}, 0);
  expect_accepts(Ptr<test_recursive>{}, 0);
  expect_accepts(Vec<test_recursive>{}, 0);
  expect_accepts(std::array<test_recursive, 0>{}, 0);
  expect_accepts(recursive_union_envelope(false), 0);
  expect_rejects(make_chain(1), 0);
  expect_rejects(std::tuple{make_chain(1)}, 0);
  expect_accepts(make_chain(1), 1);
}

TEST_P(RecursionComposition, RecursiveUnionBranchesRestoreDepth)
{
  using enum box_expr_type;
  box_expr expression(kBoxAdd);
  expression.add().emplace(std::in_place);
  (*expression.add())->left.t(kBoxLiteral);
  (*expression.add())->left.literal() = 11;
  (*expression.add())->right.t(kBoxLiteral);
  (*expression.add())->right.literal() = 22;
  // union -> recursive argument struct -> either leaf union: depth 3.
  expect_accepts(expression, 3);
  expect_rejects(expression, 2);
  expect_accepts(std::tuple{expression, expression}, 3);
}

TEST_P(RecursionComposition, MutualRecursionCountsActiveStructAndUnionEntries)
{
  static_assert(is_recursive_xdr<MutualFrame>);
  static_assert(is_recursive_xdr<MutualBody>);
  for (size_t frames : {1UZ, 2UZ, 4UZ}) {
    SCOPED_TRACE(frames);
    MutualFrame root;
    auto *frame = &root;
    for (size_t i = 0; i < frames; ++i) {
      frame->body.which(0);
      frame->body.success().x = static_cast<int>(i);
      if (i + 1 < frames) {
        frame->body.success().child.emplace();
        frame = &*frame->body.success().child;
      }
    }
    // Each frame enters MutualFrame, MutualBody, and its recursive success
    // struct. These all count while active, rather than one per cycle.
    const size_t limit = 3 * frames;
    expect_accepts(root, limit);
    expect_rejects(root, limit - 1);
    expect_accepts(std::tuple{root, root}, limit);
  }
}

TEST_P(RecursionComposition, RecursiveUnionVoidArmReleasesItsLevel)
{
  MutualFrame empty;
  empty.body.which(1); // default void arm: frame + union, no success struct
  expect_accepts(empty, 2);
  expect_rejects(empty, 1);
  expect_accepts(std::tuple{empty, empty}, 2);

  MutualFrame populated;
  populated.body.which(0);
  populated.body.success().x = 42;
  expect_accepts(populated, 3);
  expect_rejects(populated, 2);
  expect_accepts(std::tuple{empty, populated}, 3);
  expect_accepts(std::tuple{populated, empty}, 3);
}

TEST_P(RecursionComposition, FailedDecodeDoesNotAffectNextCall)
{
  expect_rejects(make_chain(5), 4);
  expect_accepts(make_chain(4), 4);
  expect_rejects(make_chain(4), 3);
  expect_accepts(make_chain(4), 4);
}

INSTANTIATE_TEST_SUITE_P(
    Paths, RecursionComposition,
    testing::Values(DecodePath::kXdr, DecodePath::kXdrFrom, DecodePath::kCxs,
                    DecodePath::kCxsFrom),
    [](const testing::TestParamInfo<DecodePath> &info) -> std::string {
      switch (info.param) {
      case DecodePath::kXdr:
        return "Xdr";
      case DecodePath::kXdrFrom:
        return "XdrFrom";
      case DecodePath::kCxs:
        return "Cxs";
      case DecodePath::kCxsFrom:
        return "CxsFrom";
      }
      return "Unknown";
    });

} // namespace
} // namespace x23
