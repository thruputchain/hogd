#include <gtest/gtest.h>

#include "hogd/init.h"

int
main(int argc, char *argv[])
{
  if (int x = hogd::hogd_init(argc, argv)) {
    return x;
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
