#include <gtest/gtest.h>

#include "hogd/hog_exec/runtime/metrics.h"
#include "hogd/hog_exec/shared.h"
#include "hogd/init.h"
#include "x23/tests/death_test.h"

namespace hogd {

// This binary deliberately does not use run_hog_exec_tests(): the point is to
// exercise launch_log_worker() in a process that forgot to call
// init_hog_exec_metrics(), which is the state the bench clients were in
// (#2605).
TEST(HogexecMetricsTest, LaunchLogWorkerRequiresInitializedMetrics)
{
  X23_EXPECT_DEATH(HogexecMetrics::launch_log_worker(), "not initialized");

  init_hog_exec_metrics();
  EXPECT_EQ(successful_validation_tx_count_metric.value(), 0U);
  EXPECT_EQ(failed_validation_tx_count_metric.value(), 0U);
  HogexecMetrics::mark_successful_validation_tx();
  HogexecMetrics::mark_failed_validation_tx();
  EXPECT_EQ(successful_validation_tx_count_metric.value(), 1U);
  EXPECT_EQ(failed_validation_tx_count_metric.value(), 1U);
  EXPECT_EQ(successful_proposed_tx_count_metric.value(), 0U);
  EXPECT_EQ(failed_proposed_tx_count_metric.value(), 0U);
  HogexecMetrics::launch_log_worker();
  // Repeat launch stays an idempotent no-op.
  HogexecMetrics::launch_log_worker();
  HogexecMetrics::stop_log_worker();
}

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
