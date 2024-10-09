#include <gtest/gtest.h>

#include "hogd/init.h"

int
main(int argc, char *argv[])
{
  if (!hogd::hogd_init(argc, argv)) {
    return 1;
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
