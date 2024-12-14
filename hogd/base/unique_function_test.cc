#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "hogd/base/unique_function.h"

namespace hogd {

TEST(UniqueFunction, Basics)
{
  // check a basic function with captures
  int foo = 0;
  UniqueFunction<void()> fn = [&foo] { foo = 1; };
  fn();
  EXPECT_EQ(foo, 1);

  // check a function with a non-copyable object
  std::string str("test");
  fn = [&foo, moved_str = std::move(str)] { foo = moved_str.size(); };
  fn();
  EXPECT_EQ(foo, 4);

  // check nullptr assignment
  fn = nullptr;
  EXPECT_FALSE(fn);

  // check a function with arguments
  UniqueFunction<void(int)> fn2 = [&foo](int n) { foo += n; };
  fn2(10);
  EXPECT_EQ(foo, 14);
}

} // namespace hogd
