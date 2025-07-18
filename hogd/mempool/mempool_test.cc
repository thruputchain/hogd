#include <cstddef>
#include <cstdio>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <iostream>

#include <map>
#include <memory>
#include <print>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/mempool.h"
#include "types.hh"

namespace hogd {

TEST(Mempool, HelloWorld)
{}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
