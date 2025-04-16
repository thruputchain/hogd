#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

#include "hogd/base/perthread.h"
#include "hogd/base/sync.h"
#include "hogd/base/types.h"
#include "hogd/metrics/metrics.h"

namespace hogd {

TEST(Metrics, BasicUsage)
{
  AtomicCounter<size_t> cnta;
  PerThreadCounter<size_t> cntb;
  size_t cntc{0};

  // register the counters
  MetricRegistry registry;
  registry.register_metrics({make_counter_metric("a", "", cnta),
                             make_counter_metric("b", "", cntb),
                             make_raw_metric("c", "", true, cntc)});

  // increment the counters
  cnta.inc(1);
  cntb.inc(2);
  cntc = 3;

  // read the samples and check them
  std::vector<Sample> samples = registry.read();
  EXPECT_EQ(samples.size(), 3);

  auto find_sample = [&samples](std::string_view key) -> std::optional<Sample> {
    auto it = std::ranges::find_if(samples, [key](const Sample &s) {
      return s.key == key;
    });
    if (it == samples.end())
      return std::nullopt;
    return *it;
  };

  std::optional<Sample> sample = find_sample("a");
  EXPECT_TRUE(sample.has_value());
  if (sample.has_value()) {
    EXPECT_EQ(sample->val, 1);
  }

  sample = find_sample("b");
  EXPECT_TRUE(sample.has_value());
  if (sample.has_value()) {
    EXPECT_EQ(sample->val, 2);
  }

  sample = find_sample("c");
  EXPECT_TRUE(sample.has_value());
  if (sample.has_value()) {
    EXPECT_EQ(sample->val, 3);
  }
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
