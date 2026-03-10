#include <gtest/gtest.h>

#include <memory>

#include "hogd/base/log.h"

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  hogd::log_init(std::make_unique<hogd::ConsoleLogger>());
  return RUN_ALL_TESTS();
}
