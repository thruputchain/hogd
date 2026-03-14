#include <gtest/gtest.h>

#include <string_view>

#include "hogd/base/log.h"

namespace hogd::detail {

TEST(LogTest, SourceBasename)
{
  EXPECT_EQ(source_basename("hogd/base/log.cc"), "log.cc");
  EXPECT_EQ(source_basename("log.cc"), "log.cc");
  EXPECT_EQ(source_basename("/tmp/foo/"), "");
  EXPECT_EQ(source_basename(""), "");
}

} // namespace hogd::detail
