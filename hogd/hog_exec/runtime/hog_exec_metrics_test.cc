#include <gtest/gtest.h>

#include "hogd/hog_exec/runtime/metrics.h"
#include "hogd/hog_exec/shared.h"
#include "hogd/init.h"

namespace hogd {

// This binary deliberately does not use run_hog_exec_tests(): the point is to
// exercise launch_log_worker() in a process that forgot to call
// init_hog_exec_metrics(), which is the state the bench clients were in
// (#2605).
TEST(HogexecMetricsTest, LaunchLogWorkerRequiresInitializedMetrics)
{
  EXPECT_DEATH(HogexecMetrics::launch_log_worker(), "not initialized");

  init_hog_exec_metrics();
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
