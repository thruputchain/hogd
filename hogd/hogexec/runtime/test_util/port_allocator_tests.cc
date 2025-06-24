#include <cstdio>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "hogd/hogexec/runtime/test_util/port_allocator.h"

namespace hogd {

TEST(PortAllocator, AllocateThenFree)
{
  std::string portfile =
      std::string(CMAKE_ROOT_BINARY_DIR) + "/test_AllocateThenFree.txt";
  // cleanup in case previous test failed and left file lying around
  std::remove(portfile.c_str());
  TemporaryPort port1(portfile);
  EXPECT_EQ(port1.port_, kMinTestPort);

  {
    TemporaryPort port2(portfile);
    EXPECT_EQ(port2.port_, kMinTestPort + 1);
    {
      TemporaryPort port3(portfile);
      EXPECT_EQ(port3.port_, kMinTestPort + 2);
    }
    TemporaryPort port4(portfile);
    EXPECT_EQ(port4.port_, kMinTestPort + 2);
  }

  auto x2 = std::make_unique<TemporaryPort>(portfile);
  EXPECT_EQ(x2->port_, kMinTestPort + 1);

  auto x3 = std::make_unique<TemporaryPort>(portfile);
  EXPECT_EQ(x3->port_, kMinTestPort + 2);
  x2.reset();
  auto x4 = std::make_unique<TemporaryPort>(portfile);
  EXPECT_EQ(x4->port_, kMinTestPort + 1);
}

} // namespace hogd
