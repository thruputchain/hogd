#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "death_test.h"
#include "x23/cleanup.h"

namespace {

class CoreLimitDeathTest : public testing::TestWithParam<const char *> {};

void
crash_with_core_dumps_disabled()
{
  rlimit limit;
  if (getrlimit(RLIMIT_CORE, &limit) != 0 || limit.rlim_cur != 0)
    _exit(0);
  std::fputs("expected crash with core dumps disabled\n", stderr);
  std::abort();
}

TEST_P(CoreLimitDeathTest, SuppressesOnlyChildCoreDumps)
{
  GTEST_FLAG_SET(death_test_style, GetParam());
  rlimit original;
  ASSERT_EQ(getrlimit(RLIMIT_CORE, &original), 0);
  if (original.rlim_max == 0)
    GTEST_SKIP() << "A nonzero parent core limit requires a nonzero hard limit";

  rlimit parent = original;
  parent.rlim_cur = std::min(parent.rlim_max, rlim_t{1024 * 1024});
  ASSERT_EQ(setrlimit(RLIMIT_CORE, &parent), 0);
  x23::Defer restore{[&] { EXPECT_EQ(setrlimit(RLIMIT_CORE, &original), 0); }};

  X23_EXPECT_DEATH(crash_with_core_dumps_disabled(), "expected crash");
  X23_ASSERT_DEATH(crash_with_core_dumps_disabled(), "expected crash");

  rlimit after;
  ASSERT_EQ(getrlimit(RLIMIT_CORE, &after), 0);
  EXPECT_EQ(after.rlim_cur, parent.rlim_cur);
  EXPECT_EQ(after.rlim_max, parent.rlim_max);
}

INSTANTIATE_TEST_SUITE_P(Styles, CoreLimitDeathTest,
                         testing::Values("fast", "threadsafe"));

} // namespace
