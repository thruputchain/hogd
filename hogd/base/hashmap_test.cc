#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>

#include "hogd/base/hashmap.h"
#include "hogd/base/types.h"

namespace hogd {

namespace {
struct ValueT {
  std::string key_;
  uint32_t value_ = 0;

  ValueT(std::string key, uint32_t value) : key_(key), value_(value) {}

  std::span<const std::byte> get_key() const
  {
    return std::as_bytes(std::span(key_));
  }

  // assert that this builds without copy on element type
  ValueT(ValueT const &) = delete;
  ValueT &operator=(ValueT const &) = delete;

  ValueT(ValueT &&) noexcept = default;
  ValueT &operator=(ValueT &&) noexcept = default;
};
} // namespace

TEST(HashMap, InsertAndGet)
{
  HashMap<ValueT> map(1);

  std::string a = "aaa", b = "bbb";

  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(a)), a, 10));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(b)), b, 12));

  auto val = map.modify_element(std::as_bytes(std::span(a)),
                                [](ValueT const &v) { return v.value_; });

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 10);
  // NOLINTEND(bugprone-unchecked-optional-access)
};

TEST(HashMap, Modify)
{
  HashMap<ValueT> map(1);

  std::string a = "aaa", b = "bbb", c = "ccc", d = "ddd", e = "eee", f = "fff";

  // there must be at least one bucket with multiple elements
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(a)), a, 10));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(b)), b, 12));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(c)), c, 14));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(d)), d, 16));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(e)), e, 18));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(f)), f, 20));

  EXPECT_FALSE(map.emplace(std::as_bytes(std::span(b)), e, 12));
  EXPECT_FALSE(map.emplace(std::as_bytes(std::span(c)), c, 12));
  EXPECT_FALSE(map.emplace(std::as_bytes(std::span(d)), d, 12));

  EXPECT_TRUE(map.modify_element(std::as_bytes(std::span(a)),
                                 [](ValueT &v) { v.value_++; }));

  auto val = map.modify_element(std::as_bytes(std::span(a)),
                                [](ValueT const &v) { return v.value_; });

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 11);
  // NOLINTEND(bugprone-unchecked-optional-access)
};

TEST(HashMap, Erase)
{
  HashMap<ValueT> map(1);

  std::string a = "aaa", b = "bbb", c = "ccc", d = "ddd", e = "eee";

  // there must be at least one bucket with multiple elements
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(a)), a, 10));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(b)), b, 12));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(c)), c, 14));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(d)), d, 16));

  EXPECT_TRUE(map.erase(std::as_bytes(std::span(a))));
  EXPECT_FALSE(map.erase(std::as_bytes(std::span(a))));

  EXPECT_FALSE(map.erase(std::as_bytes(std::span(e))));

  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(a)), a, 20));

  auto val = map.modify_element(std::as_bytes(std::span(a)),
                                [](ValueT const &v) { return v.value_; });

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 20);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

} // namespace hogd
