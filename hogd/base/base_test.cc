#include <gtest/gtest.h>

#include "hogd/base/os.h"
#include "hogd/init.h"

TEST(Os, TotalPhysicalMemoryIsPositive)
{
  // Both Linux and Darwin implement the sysconf query; a zero return would
  // mean the diagnostic in hog-devnode silently loses its RAM figure.
  EXPECT_GT(hogd::total_physical_memory(), 0U);
}

int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv, false);
  if (ret)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
