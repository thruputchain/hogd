#include <gtest/gtest.h>

#include "hogd/metrics/histogram.h"

namespace hogd {
namespace {

TEST(HistogramTest, BucketsAreLowerExclusiveUpperInclusive)
{
  HistogramBuckets<double> histogram(0.0, 5.0, 5);

  histogram.add(0.0);
  histogram.add(0.1);
  histogram.add(1.0);
  histogram.add(1.1);
  histogram.add(2.0);
  histogram.add(5.0);

  EXPECT_EQ(histogram.underflow_count(), 1);
  EXPECT_EQ(histogram.overflow_count(), 0);
  EXPECT_EQ(histogram.bucket_count(0), 2);
  EXPECT_EQ(histogram.bucket_count(1), 2);
  EXPECT_EQ(histogram.bucket_count(2), 0);
  EXPECT_EQ(histogram.bucket_count(3), 0);
  EXPECT_EQ(histogram.bucket_count(4), 1);
}

TEST(HistogramTest, ValuesOutsideRangeUseUnderflowAndOverflow)
{
  HistogramBuckets<int> histogram(0, 5, 5);

  histogram.add(-1);
  histogram.add(0);
  histogram.add(6);

  EXPECT_EQ(histogram.underflow_count(), 2);
  EXPECT_EQ(histogram.overflow_count(), 1);
}

} // namespace
} // namespace hogd
