#include <gtest/gtest.h>

#include "hogd/base/atomic.h"
#include "hogd/base/types.h"

namespace hogd {

// GccAtomic16B is only used on platforms where std::atomic<int128_t> does not
// provide fetch_add (i.e., GCC). On Clang, int128_t atomics are always
// lock-free and have integral specializations, so GccAtomic16B cannot be
// instantiated due to its static_assert.
//
// On platforms where GccAtomic16B is not used (e.g., Clang/macOS), we verify
// that the Atomic16B alias (std::atomic) operator+= / operator-= work
// correctly.
TEST(Atomic16BTest, OperatorPlusEqualsReturnsNewValue)
{
  Atomic16B<int128_t> a(int128_t{10});
  int128_t result = (a += int128_t{5});
  // operator+= should return the NEW value (old + arg), i.e. 10 + 5 = 15
  EXPECT_EQ(result, int128_t{15});
  EXPECT_EQ(a.load(), int128_t{15});
}

TEST(Atomic16BTest, OperatorMinusEqualsReturnsNewValue)
{
  Atomic16B<int128_t> a(int128_t{10});
  int128_t result = (a -= int128_t{3});
  // operator-= should return the NEW value (old - arg), i.e. 10 - 3 = 7
  EXPECT_EQ(result, int128_t{7});
  EXPECT_EQ(a.load(), int128_t{7});
}

TEST(Atomic16BTest, OperatorPlusEqualsWithLargeArg)
{
  Atomic16B<int128_t> a(int128_t{100});
  int128_t result = (a += int128_t{200});
  // Should return 100 + 200 = 300, not 100 + 1 = 101
  EXPECT_EQ(result, int128_t{300});
  EXPECT_EQ(a.load(), int128_t{300});
}

TEST(Atomic16BTest, OperatorMinusEqualsWithLargeArg)
{
  Atomic16B<int128_t> a(int128_t{500});
  int128_t result = (a -= int128_t{200});
  // Should return 500 - 200 = 300, not 500 - 1 = 499
  EXPECT_EQ(result, int128_t{300});
  EXPECT_EQ(a.load(), int128_t{300});
}

TEST(Atomic16BTest, FetchAddWrapsAcrossInt128Max)
{
  Atomic16B<int128_t> a(std::numeric_limits<int128_t>::max());
  int128_t old = a.fetch_add(int128_t{100});

  EXPECT_EQ(old, std::numeric_limits<int128_t>::max());
  EXPECT_EQ(a.load(), std::numeric_limits<int128_t>::min() + 99);
}

TEST(Atomic16BTest, FetchSubWrapsAcrossInt128Min)
{
  Atomic16B<int128_t> a(std::numeric_limits<int128_t>::min());
  int128_t old = a.fetch_sub(int128_t{1});

  EXPECT_EQ(old, std::numeric_limits<int128_t>::min());
  EXPECT_EQ(a.load(), std::numeric_limits<int128_t>::max());
}

} // namespace hogd
