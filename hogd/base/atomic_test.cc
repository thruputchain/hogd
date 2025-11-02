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

// Explicitly tests the gcc fallback implementation on both compilers.
TEST(AtomicTest, GccAtomic128BasicUsage)
{
  using gcc_fallback_t = detail::GccAtomic16B<int128_t>;

  gcc_fallback_t gcc_atomic(0);
  EXPECT_EQ(gcc_atomic.load(), 0);
  gcc_atomic.store(1);
  EXPECT_EQ(gcc_atomic.load(), 1);
  gcc_atomic.exchange(2);
  EXPECT_EQ(gcc_atomic.load(), 2);

  int128_t expected = 2;
  EXPECT_TRUE(gcc_atomic.compare_exchange_strong(expected, 3));

  const gcc_fallback_t gcc_atomic_const(100);
  EXPECT_EQ(gcc_atomic_const.load(), 100);
}

} // namespace hogd
