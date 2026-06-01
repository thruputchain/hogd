#include <gtest/gtest.h>

#include "hogd/init.h"

int
main(int argc, char *argv[])
{
  const int init_result = hogd::hogd_init(argc, argv, false);
  if (init_result != 0) {
    return init_result;
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
