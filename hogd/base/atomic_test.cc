#include <gtest/gtest.h>

#include "hogd/base/atomic.h"
#include "hogd/base/types.h"

namespace hogd {

TEST(AtomicTest, Basic)
{
  hogd::Atomic16B<int128_t> atomic(0);
  EXPECT_EQ(atomic.load(), 0);
  atomic.store(1);
  EXPECT_EQ(atomic.load(), 1);
  atomic.exchange(2);
  EXPECT_EQ(atomic.load(), 2);

  int128_t expected = 2;
  EXPECT_TRUE(atomic.compare_exchange_strong(expected, 3));
  EXPECT_EQ(expected, 2);
  EXPECT_FALSE(atomic.compare_exchange_strong(expected, 4));
}

} // namespace hogd
