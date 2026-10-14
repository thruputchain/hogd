#include <gtest/gtest.h>

#include <cmath>

#include "hogd/metrics/histogram.h"
#include "x23/tests/death_test.h"

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

TEST(HistogramImplTest, BoundsAreInclusiveAndOverflowIsOnlyInInf)
{
  // Binary-exact values, so _sum formats without rounding noise.
  HistogramImpl histogram("latency", {0.25, 0.5, 1});

  histogram.record(0.125);
  histogram.record(0.25);
  histogram.record(0.375);
  histogram.record(1);
  histogram.record(2.5);

  EXPECT_EQ(histogram.dump_sample("lat"), "lat_bucket{le=\"0.25\"} 2\n"
                                          "lat_bucket{le=\"0.5\"} 3\n"
                                          "lat_bucket{le=\"1\"} 4\n"
                                          "lat_bucket{le=\"+Inf\"} 5\n"
                                          "lat_sum 4.25\n"
                                          "lat_count 5\n");
}

TEST(HistogramImplTest, CountAndSumReadBackWhatWasRecorded)
{
  // The accessors let callers assert on a distribution without parsing a
  // scrape; they must agree with the _count and _sum the scrape renders.
  HistogramImpl histogram("latency", {0.25, 0.5, 1});

  EXPECT_EQ(histogram.count(), 0UL);
  EXPECT_EQ(histogram.sum(), 0.0);

  histogram.record(0.125);
  histogram.record(2.5);
  histogram.record(std::nan(""));

  EXPECT_EQ(histogram.count(), 2UL);
  EXPECT_EQ(histogram.sum(), 2.625);
}

TEST(HistogramImplTest, EmptyHistogramExposesEverySeriesAtZero)
{
  HistogramImpl histogram("latency", {0.5, 2});

  EXPECT_EQ(histogram.dump_sample("lat"), "lat_bucket{le=\"0.5\"} 0\n"
                                          "lat_bucket{le=\"2\"} 0\n"
                                          "lat_bucket{le=\"+Inf\"} 0\n"
                                          "lat_sum 0\n"
                                          "lat_count 0\n");
}

TEST(HistogramImplTest, SmallBoundsUseFixedPoint)
{
  HistogramImpl histogram("latency", {0.0001, 0.0005, 60});

  EXPECT_EQ(histogram.dump_sample("lat"), "lat_bucket{le=\"0.0001\"} 0\n"
                                          "lat_bucket{le=\"0.0005\"} 0\n"
                                          "lat_bucket{le=\"60\"} 0\n"
                                          "lat_bucket{le=\"+Inf\"} 0\n"
                                          "lat_sum 0\n"
                                          "lat_count 0\n");
}

TEST(HistogramImplTest, NanIsIgnored)
{
  HistogramImpl histogram("latency", {1});

  histogram.record(std::nan(""));
  histogram.record(0.5);

  EXPECT_EQ(histogram.dump_sample("lat"), "lat_bucket{le=\"1\"} 1\n"
                                          "lat_bucket{le=\"+Inf\"} 1\n"
                                          "lat_sum 0.5\n"
                                          "lat_count 1\n");
}

TEST(HistogramImplDeathTest, RejectsInvalidBounds)
{
  X23_EXPECT_DEATH(HistogramImpl("latency", {}), "CHECK FAILED");
  X23_EXPECT_DEATH(HistogramImpl("latency", {1, 1}), "CHECK FAILED");
  X23_EXPECT_DEATH(HistogramImpl("latency", {2, 1}), "CHECK FAILED");
  X23_EXPECT_DEATH(HistogramImpl("latency", {1, INFINITY}), "CHECK FAILED");
  // Too wide to print in fixed point: caught at registration, not on scrape.
  X23_EXPECT_DEATH(HistogramImpl("latency", {1, 1e70}), "CHECK FAILED");
}

} // namespace
} // namespace hogd
