#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "x23/vec.h"

using namespace x23;

static_assert(
    !noexcept(std::declval<Vec<int, 2> &>() = std::declval<Vec<int, 4> &&>()));
static_assert(
    noexcept(std::declval<Vec<int, 4> &>() = std::declval<Vec<int, 2> &&>()));
static_assert(std::same_as<decltype(Str("a") + Str("b")), Str>);
static_assert(std::is_nothrow_move_constructible_v<Str>);
static_assert(std::is_nothrow_move_assignable_v<Str>);

#if !BROKEN_P2564R3
static_assert([] {
  Vec<int> values{1};
  values.push_back(values.front());
  return values == Vec<int>{1, 1};
}());
#endif

TEST(VecBounds, NarrowingMoveRejectsOversize)
{
  Vec<int, 2> dest{9};
  Vec<int, 4> source{1, 2, 3};
  auto *data = source.data();
  EXPECT_THROW(dest = std::move(source), XdrLengthError);
  EXPECT_EQ(dest, (Vec<int, 2>{9}));
  EXPECT_EQ(source, (Vec<int, 4>{1, 2, 3}));
  EXPECT_EQ(source.data(), data);
}

TEST(VecBounds, MoveBetweenBounds)
{
  Vec<int, 2> small{1, 2};
  Vec<int, 4> large{9};
  auto *data = small.data();
  large = std::move(small);
  EXPECT_TRUE(small.empty());
  EXPECT_EQ(large.data(), data);
  EXPECT_EQ(large, (Vec<int, 4>{1, 2}));
  small = std::move(large);
  EXPECT_TRUE(large.empty());
  EXPECT_EQ(small.data(), data);
  EXPECT_EQ(small, (Vec<int, 2>{1, 2}));
}

TEST(VecBounds, NarrowingConstructorPreservesSourceOnFailure)
{
  Vec<int, 4> source{1, 2, 3};
  EXPECT_THROW((Vec<int, 2>{std::move(source)}), XdrLengthError);
  EXPECT_EQ(source, (Vec<int, 4>{1, 2, 3}));
}

TEST(VecBounds, OverwriteAtMaximumSize)
{
  Vec<uint8_t, 4> bytes{1, 2};
  bytes.resize_and_overwrite(4, [](uint8_t *p, size_t count) {
    EXPECT_EQ(count, 4UZ);
    EXPECT_EQ(p[0], 1);
    EXPECT_EQ(p[1], 2);
    p[2] = 3;
    p[3] = 4;
    return count;
  });
  EXPECT_EQ(bytes, (Vec<uint8_t, 4>{1, 2, 3, 4}));

  BoundedStr<4> text = "ab";
  uformat_to(&text, "{}", "cd");
  EXPECT_EQ(std::string_view(text), "abcd");
  EXPECT_EQ(text.data()[text.size()], '\0');
}

TEST(VecBounds, OverwriteEmptyAndShrink)
{
  Vec<uint8_t, 0> empty;
  bool called = false;
  empty.resize_and_overwrite(0, [&](uint8_t *, size_t count) {
    called = true;
    EXPECT_EQ(count, 0UZ);
    return 0;
  });
  EXPECT_TRUE(called);
  EXPECT_TRUE(empty.empty());

  Str text = "abcdef";
  text.resize_and_overwrite(8, [](char *p, size_t count) {
    EXPECT_EQ(count, 8UZ);
    EXPECT_EQ(std::string_view(p, 6), "abcdef");
    p[0] = 'X';
    return 3;
  });
  EXPECT_EQ(std::string_view(text), "Xbc");
  EXPECT_EQ(text.data()[text.size()], '\0');
}

TEST(VecBounds, OverwriteRejectsOversizeBeforeCallback)
{
  Vec<uint8_t, 4> bytes{1, 2};
  bool called = false;
  EXPECT_THROW(bytes.resize_and_overwrite(5,
                                          [&](uint8_t *, size_t) {
                                            called = true;
                                            return 0;
                                          }),
               XdrLengthError);
  EXPECT_FALSE(called);
  EXPECT_EQ(bytes, (Vec<uint8_t, 4>{1, 2}));
}

TEST(VecAlias, CopyElementDuringGrowth)
{
  Vec<std::string> values;
  values.emplace_back(128, 'x');
  values.shrink_to_fit();
  std::string expected = values.front();
  values.push_back(values.front());
  ASSERT_EQ(values.size(), 2UZ);
  EXPECT_EQ(values[0], expected);
  EXPECT_EQ(values[1], expected);
}

TEST(VecAlias, MoveElementDuringGrowth)
{
  Vec<std::unique_ptr<int>> values;
  values.push_back(std::make_unique<int>(42));
  values.shrink_to_fit();
  auto *ptr = values.front().get();
  values.push_back(std::move(values.front()));
  ASSERT_EQ(values.size(), 2UZ);
  EXPECT_FALSE(values[0]);
  ASSERT_TRUE(values[1]);
  EXPECT_EQ(values[1].get(), ptr);
  EXPECT_EQ(*values[1], 42);
}

TEST(VecAlias, ConstructFromElementDataDuringGrowth)
{
  Vec<std::string> values;
  values.emplace_back(128, 'y');
  values.shrink_to_fit();
  values.emplace_back(values.front().data() + 1, 100);
  ASSERT_EQ(values.size(), 2UZ);
  EXPECT_EQ(values[0], std::string(128, 'y'));
  EXPECT_EQ(values[1], std::string(100, 'y'));
}

TEST(StrValueCategory, SubstringsCopyLvalues)
{
  Str text = "abcdef";
  auto sub = text.substr(1, 3);
  static_assert(std::same_as<decltype(sub), Str>);
  EXPECT_EQ(std::string_view(sub), "bcd");
  sub[0] = 'X';
  EXPECT_EQ(std::string_view(text), "abcdef");
  const Str &constant = text;
  EXPECT_EQ(std::string_view(constant.substr(2)), "cdef");
  EXPECT_TRUE(text.substr(text.size()).empty());
  EXPECT_TRUE(constant.substr(999).empty());
  EXPECT_TRUE(text.substr(0, 0).empty());
}

TEST(StrValueCategory, SubstringsMoveRvalues)
{
  Str text = "abcdef";
  auto *data = text.data();
  auto sub = std::move(text).substr(1, 3);
  EXPECT_EQ(sub.data(), data);
  EXPECT_EQ(std::string_view(sub), "bcd");
  EXPECT_EQ(sub.data()[sub.size()], '\0');
  EXPECT_TRUE(text.empty());
  auto prefix = std::move(sub).substr(0, 2);
  EXPECT_EQ(prefix.data(), data);
  EXPECT_EQ(std::string_view(prefix), "bc");
  EXPECT_TRUE(sub.empty());
  EXPECT_TRUE(Str("abc").substr(999).empty());
}

TEST(StrValueCategory, ConcatenationCopiesLvalues)
{
  Str left = "left";
  Str right = "right";
  auto joined = left + right;
  static_assert(std::same_as<decltype(joined), Str>);
  EXPECT_EQ(std::string_view(joined), "leftright");
  const Str &constant = left;
  EXPECT_EQ(std::string_view(constant + right), "leftright");
  EXPECT_EQ(std::string_view(left), "left");
  EXPECT_EQ(std::string_view(right), "right");
}

TEST(VecValueCategory, ConcatenationCopiesLvalues)
{
  Vec<int> left{1, 2};
  const Vec<int> right{3, 4};
  auto joined = left + right;
  static_assert(std::same_as<decltype(joined), Vec<int>>);
  EXPECT_EQ(joined, (Vec<int>{1, 2, 3, 4}));
  EXPECT_EQ(left, (Vec<int>{1, 2}));
  EXPECT_EQ(right, (Vec<int>{3, 4}));
}

TEST(VecAlias, FailedConstructionPreservesElements)
{
  Vec<std::string> values;
  values.emplace_back(128, 'z');
  values.shrink_to_fit();
  auto *data = values.data();
  EXPECT_THROW(values.emplace_back(values.front(), 129), std::out_of_range);
  EXPECT_EQ(values.data(), data);
  ASSERT_EQ(values.size(), 1UZ);
  EXPECT_EQ(values.front(), std::string(128, 'z'));
}

TEST(VecBounds, FailedGrowthDoesNotMoveArgument)
{
  Vec<std::unique_ptr<int>, 1> values;
  values.push_back(std::make_unique<int>(42));
  auto *ptr = values.front().get();
  EXPECT_THROW(values.push_back(std::move(values.front())), XdrLengthError);
  ASSERT_EQ(values.size(), 1UZ);
  EXPECT_EQ(values.front().get(), ptr);
  EXPECT_EQ(*values.front(), 42);
}

TEST(StrValueCategory, BoundedResultsRetainBound)
{
  BoundedStr<4> text = "abcd";
  auto sub = text.substr(1, 2);
  static_assert(std::same_as<decltype(sub), BoundedStr<4>>);
  EXPECT_EQ(std::string_view(sub), "bc");
  EXPECT_THROW(text + std::string_view("e"), XdrLengthError);
  EXPECT_EQ(std::string_view(text), "abcd");
}

TEST(StrValueCategory, MoveTransfersStorage)
{
  BoundedStr<8> source = "text";
  auto *data = source.data();
  BoundedStr<8> moved(std::move(source));
  EXPECT_TRUE(source.empty());
  EXPECT_EQ(moved.data(), data);
  EXPECT_EQ(std::string_view(moved), "text");
  EXPECT_EQ(moved.data()[moved.size()], '\0');
  BoundedStr<8> assigned = "old";
  assigned = std::move(moved);
  EXPECT_TRUE(moved.empty());
  EXPECT_EQ(assigned.data(), data);
  EXPECT_EQ(std::string_view(assigned), "text");
  auto &alias = assigned;
  assigned = std::move(alias);
  EXPECT_EQ(assigned.data(), data);
  EXPECT_EQ(std::string_view(assigned), "text");
}

TEST(VecBounds, OverwriteRestoresTerminator)
{
  BoundedStr<4> text = "abcd";
  text.resize_and_overwrite(4, [](char *p, size_t count) {
    p[count] = 'X';
    return count;
  });
  EXPECT_EQ(std::string_view(text), "abcd");
  EXPECT_EQ(text.data()[text.size()], '\0');
}

namespace {

template<typename T> struct FailingAllocator {
  using value_type = T;
  bool *fail_{};

  T *allocate(size_t n)
  {
    if (fail_ && *fail_)
      throw std::bad_alloc{};
    return std::allocator<T>{}.allocate(n);
  }
  void deallocate(T *p, size_t n) noexcept
  {
    std::allocator<T>{}.deallocate(p, n);
  }
};

} // namespace

TEST(VecAlias, AllocationFailureDoesNotMoveArgument)
{
  bool fail = false;
  using Element = std::unique_ptr<int>;
  Vec<Element, 4, FailingAllocator<Element>> values({&fail});
  values.push_back(std::make_unique<int>(42));
  values.shrink_to_fit();
  auto *ptr = values.front().get();
  fail = true;
  EXPECT_THROW(values.push_back(std::move(values.front())), std::bad_alloc);
  ASSERT_EQ(values.size(), 1UZ);
  ASSERT_EQ(values.front().get(), ptr);
  EXPECT_EQ(*values.front(), 42);
}
