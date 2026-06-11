#pragma once

#include <gtest/gtest.h>

#include "hogd/hogexec/shared.h"
#include "hogd/init.h"

namespace hogd {

inline int
run_hogexec_tests(int argc, char *argv[])
{
  auto ret = hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;

  init_metrics();

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

} // namespace hogd
