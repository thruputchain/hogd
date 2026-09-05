#include <gtest/gtest.h>

#include <span>
#include <string_view>

#include "hogd/hog_db/item_change.h"

namespace hogd {

TEST(ItemChangeExclusiveTest, TestNarrow)
{
  using namespace std::literals;
  ItemChange change1(Bytes(std::as_bytes(std::span("hello"sv))));

  ItemChange change2(Bytes(std::as_bytes(std::span("hello world"sv))));

  ASSERT_FALSE(change1.is_exclusive());

  EXPECT_TRUE(change1.narrow(change2).has_value());
  ASSERT_TRUE(change1.is_exclusive());

  // the same as change1
  ItemChange change3(Bytes(std::as_bytes(std::span("hello"sv))));
  ASSERT_FALSE(change3.is_exclusive());

  EXPECT_TRUE(change1.narrow(change3).has_value());
  ASSERT_TRUE(change1.is_exclusive());
  EXPECT_TRUE(change1.narrow(change3).has_value());
  ASSERT_TRUE(change1.is_exclusive());
}

} // namespace hogd
