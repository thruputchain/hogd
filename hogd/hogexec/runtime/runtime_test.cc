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

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(RuntimeTest, MethodIDFormat)
{
  auto id = Runtime::method_id_from_name("0000000000000000");
  ASSERT_TRUE(id.has_value());
  ASSERT_EQ(id, 0);

  ASSERT_EQ(Runtime::invoke_name(0), "0000000000000000");
  ASSERT_EQ(Runtime::invoke_name(0x0000'0678'90AB'CDEF), "0000067890ABCDEF");
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
