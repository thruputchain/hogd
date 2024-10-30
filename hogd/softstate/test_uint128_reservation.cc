#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "hogd/softstate/uint128_reservation.h"

namespace hogd {

TEST(UInt128Reservation, BasicCorrectness)
{
  UInt128Reservation v;

  v += UINT64_MAX;
  v += UINT64_MAX;
  v += UINT64_MAX;

  EXPECT_EQ(v.get_capped(), UINT64_MAX);

  v -= UINT64_MAX;
  v -= 10;

  EXPECT_EQ(v.get_capped(), UINT64_MAX);

  v -= UINT64_MAX;
  EXPECT_EQ(v.get_capped(), UINT64_MAX - 10);
}

} // namespace hogd
