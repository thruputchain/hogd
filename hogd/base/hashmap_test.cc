#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include "hogd/base/hashmap.h"
#include "hogd/base/types.h"

namespace hogd {

namespace {
struct ValueT {
  std::string key_;
  uint32_t value_ = 0;

  ValueT(std::string key, uint32_t value) : key_(std::move(key)), value_(value)
  {}

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
namespace {
template<typename Map>
void
insert_and_get_test(Map &map)
{
  std::string a = "aaa", b = "bbb", c = "ccc";

  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(a)), a, 10));
  EXPECT_TRUE(map.emplace(std::as_bytes(std::span(b)), b, 12));

  auto val = map.modify_element(std::as_bytes(std::span(a)),
                                [](ValueT const &v) { return v.value_; });

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 10);
  // NOLINTEND(bugprone-unchecked-optional-access)

  // asserting that emplace and modify compiles and doesn't emplace
  // when an element already exists
  val = map.emplace_and_modify(
      std::as_bytes(std::span(a)), [](ValueT const &v) { return v.value_; }, a,
      5);

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 10);
  // NOLINTEND(bugprone-unchecked-optional-access)

  // asserting that emplace and modify compiles and does emplace
  // when an element does not already exist
  val = map.emplace_and_modify(
      std::as_bytes(std::span(c)),
      [](ValueT &v) {
        v.value_++;
        return v.value_;
      },
      c, 5);

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(val);
  EXPECT_EQ(*val, 6);
  // NOLINTEND(bugprone-unchecked-optional-access)
}
} // namespace

TEST(HashMap, InsertAndGet)
{
  HashMap<ValueT, vector_bucket_t<ValueT>> vector_map(1);
  insert_and_get_test(vector_map);

  HashMap<ValueT, set_bucket_t<ValueT>> set_map(1);
  insert_and_get_test(set_map);
};

namespace {
template<typename Map>
void
modify_test(Map &map)
{
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
}
} // namespace

TEST(HashMap, Modify)
{
  HashMap<ValueT, vector_bucket_t<ValueT>> vector_map(1);
  modify_test(vector_map);

  HashMap<ValueT, set_bucket_t<ValueT>> set_map(1);
  modify_test(set_map);
};

namespace {
template<typename Map>
void
erase_test(Map &map)
{
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
} // namespace

TEST(HashMap, Erase)
{
  HashMap<ValueT, vector_bucket_t<ValueT>> vector_map(1);
  erase_test(vector_map);

  HashMap<ValueT, set_bucket_t<ValueT>> set_map(1);
  erase_test(set_map);
}

} // namespace hogd
