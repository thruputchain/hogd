#include <gtest/gtest.h>

#include <cerrno>
#include <format>
#include <system_error>

#include <fcntl.h>

#include "hogd/base/status.h"

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

  try {
    ret.error().raise();
  } catch (const std::system_error &e) {
    EXPECT_EQ(e.code(), ret.error());
    EXPECT_EQ(e.code(), GCode::kInvalidArgument);
  }
}

TEST(StatusTest, Errno)
{
  int ret = open("/", O_RDWR); // NOLINT(misc-include-cleaner)
  EXPECT_EQ(ret, -1);
  Errno err;
  EXPECT_EQ(err, EISDIR);
}

} // namespace hogd
