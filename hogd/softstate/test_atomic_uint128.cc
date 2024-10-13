#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "hogd/softstate/atomic_uint128.h"

namespace hogd {

TEST(AtomicUint128, BasicCorrectness)
{
  AtomicUint128 v;

  v.add(UINT64_MAX);
  v.add(UINT64_MAX);
  v.add(UINT64_MAX);

  EXPECT_EQ(v.get_capped(), UINT64_MAX);

  v.sub(UINT64_MAX);
  v.sub(10);

  EXPECT_EQ(v.get_capped(), UINT64_MAX);

  v.sub(UINT64_MAX);
  EXPECT_EQ(v.get_capped(), UINT64_MAX - 10);
}

} // namespace hogd
