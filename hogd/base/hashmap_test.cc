#include <gtest/gtest.h>

#include "hogd/base/hashmap.h"

namespace hogd {

namespace {
struct value_t {
  std::string key;
  uint32_t value = 0;

  std::span<const std::byte> get_key() const
  {
    return std::as_bytes(std::span(key));
  }
};
} // namespace

TEST(HashMap, InsertAndGet)
{
  HashMap<value_t> map(1, 1);

  std::string a = "aaa", b = "bbb";

  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(a)), a, 10));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(b)), b, 12));

  auto val = map.modify_element(std::as_bytes(std::span(a)),
                                [](value_t const &v) { return v.value; });

  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 10);
};

TEST(HashMap, Modify)
{
  HashMap<value_t> map(1, 1);

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
                                 [](value_t &v) { v.value++; }));

  auto val = map.modify_element(std::as_bytes(std::span(a)),
                                [](value_t const &v) { return v.value; });

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 11);
  // NOLINTEND(bugprone-unchecked-optional-access)
};

} // namespace hogd
