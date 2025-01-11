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

  EXPECT_TRUE(map.emplace(std::as_bytes(std::span("aaa")), "aaa", 10));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span("bbb")), "bbb", 12));

  auto val = map.modify_element(std::as_bytes(std::span("aaa")),
                                [](value_t const &v) { return v.value; });

  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 10);
};

TEST(HashMap, Modify)
{
  HashMap<value_t> map(1, 1);

  // there must be at least one bucket with multiple elements
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span("aaa")), "aaa", 10));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span("bbb")), "bbb", 12));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span("ccc")), "ccc", 14));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span("ddd")), "ddd", 16));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span("eee")), "eee", 18));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span("fff")), "fff", 20));

  EXPECT_FALSE(map.emplace(std::as_bytes(std::span("bbb")), "bbb", 12));
  EXPECT_FALSE(map.emplace(std::as_bytes(std::span("ccc")), "ccc", 12));
  EXPECT_FALSE(map.emplace(std::as_bytes(std::span("ddd")), "ddd", 12));

  EXPECT_TRUE(map.modify_element(std::as_bytes(std::span("aaa")),
                                 [](value_t &v) { v.value++; }));

  auto val = map.modify_element(std::as_bytes(std::span("aaa")),
                                [](value_t const &v) { return v.value; });

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 11);
  // NOLINTEND(bugprone-unchecked-optional-access)
};

} // namespace hogd
