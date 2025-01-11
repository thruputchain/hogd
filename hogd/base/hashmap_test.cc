#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>

#include "hogd/base/hashmap.h"
#include "hogd/base/types.h"

namespace hogd {

namespace {
struct ValueT {
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
  HashMap<ValueT> map(1, 1);

  std::string a = "aaa", b = "bbb";

  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(a)), a, 10));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(b)), b, 12));

  auto val = map.modify_element(std::as_bytes(std::span(a)),
                                [](ValueT const &v) { return v.value; });

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 10);
  // NOLINTEND(bugprone-unchecked-optional-access)
};

TEST(HashMap, Modify)
{
  HashMap<ValueT> map(1, 1);

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
                                 [](ValueT &v) { v.value++; }));

  auto val = map.modify_element(std::as_bytes(std::span(a)),
                                [](ValueT const &v) { return v.value; });

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 11);
  // NOLINTEND(bugprone-unchecked-optional-access)
};

} // namespace hogd
