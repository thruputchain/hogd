#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/init.h"
#include "types.hh"

namespace hogd {

TEST(RuntimeTest, MethodIDRoundTrip)
{
  auto check_round_trip = [](MethodID id) {
    auto name = Runtime::invoke_name(id);
    auto res = Runtime::method_id_from_name(name);
    ASSERT_EQ(res, id);
  };

  check_round_trip(0x1234'5678'90AB'CDEF);
  check_round_trip(0);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
