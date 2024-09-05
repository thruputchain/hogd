#include <cerrno>
#include <format>

#include "hogd/base/status.h"

#include <fcntl.h>
#include <gtest/gtest.h>

namespace hogd {

namespace {

inline constexpr int kThreshold = 10;

GStatus<int>
foo(int i)
{
  if (i >= kThreshold)
    return GError(GCode::kInvalidArgument);
  return i;
}

} // namespace

TEST(StatusTest, EnumError)
{
  GStatus<int> ret = foo(0);
  EXPECT_TRUE(ret);
  EXPECT_EQ(*ret, 0);

  ret = foo(kThreshold);
  EXPECT_FALSE(ret);
  EXPECT_EQ(ret.error(), GCode::kInvalidArgument);
  EXPECT_EQ(std::format("{}", ret.error()), "INVALID_ARGUMENT");
}

TEST(StatusTest, Errno)
{
  int ret = open("/", O_RDWR);
  EXPECT_EQ(ret, -1);
  Errno err;
  EXPECT_EQ(err, EISDIR);
}

} // namespace hogd
