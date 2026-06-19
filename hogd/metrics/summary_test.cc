#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/metrics/summary.h"

namespace hogd {
namespace {

TEST(SummaryTest, DumpsConfiguredQuantilesSumAndCount)
{
  SummaryBuckets<size_t> summary(0, 10, 5, {0.5, 0.9});

  summary.add(0);
  summary.add(1);
  summary.add(3);
  summary.add(7);
  summary.add(9);

  EXPECT_EQ(summary.count(), 5);
  EXPECT_EQ(summary.sum(), 20);
  EXPECT_EQ(summary.quantile_upper_bound(0.5), "4");
  EXPECT_EQ(summary.quantile_upper_bound(0.9), "10");
  EXPECT_EQ(summary.dump_to_string("validation_latency_ms"),
            "validation_latency_ms{quantile=\"0.5\"} 4\n"
            "validation_latency_ms{quantile=\"0.9\"} 10\n"
            "validation_latency_ms_sum 20\n"
            "validation_latency_ms_count 5\n");
}

TEST(SummaryTest, EmptySummaryDumpsNanQuantile)
{
  SummaryBuckets<size_t> summary(0, 10, 5, {0.5});

  EXPECT_EQ(summary.count(), 0);
  EXPECT_EQ(summary.sum(), 0);
  EXPECT_EQ(summary.quantile_upper_bound(0.5), "NaN");
}

} // namespace
} // namespace hogd
