#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/http/http.h"
#include "hogd/http/http_test.h"
#include "hogd/metrics/new_metrics.h"
#include "hogd/metrics/shared.h"

namespace hogd {
namespace {

class NewMetricsTest : public ::testing::Test {
protected:
  void SetUp() override { Metrics::instance().clear()(); }
};

std::vector<std::string_view>
get_lines(std::string_view input)
{
  std::vector<std::string_view> result;
  while (!input.empty()) {
    const size_t newline = input.find('\n');
    if (newline == std::string_view::npos) {
      result.push_back(input);
      break;
    }
    result.push_back(input.substr(0, newline));
    input.remove_prefix(newline + 1);
  }
  return result;
}

void
expect_dump_contains(std::string_view dump, std::string_view expected)
{
  const std::vector<std::string_view> dump_lines = get_lines(dump);
  EXPECT_TRUE(std::ranges::contains(dump_lines, expected))
      << "missing line: " << expected << "\ndump:\n"
      << dump;
}

TEST_F(NewMetricsTest, InstanceReturnsSingleton)
{
  EXPECT_EQ(&Metrics::instance(), &Metrics::instance());
}

TEST_F(NewMetricsTest, ComponentNamesFormatAsPrometheusPrefixes)
{
  EXPECT_EQ(std::format("{}", Metrics::ComponentName::kExecutor), "executor");
  EXPECT_EQ(std::format("{}", Metrics::ComponentName::kDb), "db");
  EXPECT_EQ(std::format("{}", Metrics::ComponentName::kMempool), "mempool");
}

TEST_F(NewMetricsTest, DumpIncludesRecordedGaugeForComponent)
{
  Metrics &metrics = Metrics::instance();

  metrics.record(Metrics::ComponentName::kExecutor,
                 "metrics_test_executor_queue_depth", 17)();

  const std::string dump = metrics.dump_to_prometheus_string()();
  expect_dump_contains(
      dump, "# TYPE executor_metrics_test_executor_queue_depth gauge");
  expect_dump_contains(dump, "executor_metrics_test_executor_queue_depth 17");
}

TEST_F(NewMetricsTest, DumpIncludesDescriptionForDescribedMetric)
{
  Metrics &metrics = Metrics::instance();

  metrics.set_description(Metrics::ComponentName::kMempool,
                          "metrics_test_described_mempool_depth",
                          "Number of transactions waiting in the mempool");
  metrics.record(Metrics::ComponentName::kMempool,
                 "metrics_test_described_mempool_depth", 42)();

  const std::string dump = metrics.dump_to_prometheus_string()();
  expect_dump_contains(dump,
                       "# HELP mempool_metrics_test_described_mempool_depth "
                       "Number of transactions "
                       "waiting in the mempool");
  expect_dump_contains(
      dump, "# TYPE mempool_metrics_test_described_mempool_depth gauge");
  expect_dump_contains(dump, "mempool_metrics_test_described_mempool_depth 42");
}

TEST_F(NewMetricsTest, DumpOmitsMissingDescription)
{
  Metrics &metrics = Metrics::instance();

  metrics.record(Metrics::ComponentName::kDb,
                 "metrics_test_unrecorded_db_snapshot_count", 5)();

  const std::string dump = metrics.dump_to_prometheus_string()();
  const std::vector<std::string_view> dump_lines = get_lines(dump);
  EXPECT_FALSE(std::ranges::contains(
      dump_lines, "# HELP db_metrics_test_unrecorded_db_snapshot_count "
                  "Snapshots known to the "
                  "database"))
      << dump;
  expect_dump_contains(
      dump, "# TYPE db_metrics_test_unrecorded_db_snapshot_count gauge");
  expect_dump_contains(dump, "db_metrics_test_unrecorded_db_snapshot_count 5");
}

TEST_F(NewMetricsTest, DumpOmitsDescriptionForUnrecordedMetric)
{
  Metrics &metrics = Metrics::instance();

  metrics.set_description(Metrics::ComponentName::kDb,
                          "metrics_test_unrecorded_db_snapshot_count",
                          "Snapshots known to the database");

  const std::string dump = metrics.dump_to_prometheus_string()();
  const std::vector<std::string_view> dump_lines = get_lines(dump);
  EXPECT_FALSE(std::ranges::contains(
      dump_lines, "# HELP db_metrics_test_unrecorded_db_snapshot_count "
                  "Snapshots known to the "
                  "database"))
      << dump;
  EXPECT_FALSE(std::ranges::contains(
      dump_lines, "# TYPE db_metrics_test_unrecorded_db_snapshot_count gauge"))
      << dump;
}

TEST_F(NewMetricsTest, RecordingSameMetricUpdatesLatestSample)
{
  Metrics &metrics = Metrics::instance();

  metrics.record(Metrics::ComponentName::kDb, "metrics_test_db_snapshot_count",
                 3)();
  metrics.record(Metrics::ComponentName::kDb, "metrics_test_db_snapshot_count",
                 9)();

  const std::string dump = metrics.dump_to_prometheus_string()();
  expect_dump_contains(dump, "db_metrics_test_db_snapshot_count 9");

  const std::vector<std::string_view> dump_lines = get_lines(dump);
  EXPECT_FALSE(
      std::ranges::contains(dump_lines, "db_metrics_test_db_snapshot_count 3"))
      << dump;
}

TEST_F(NewMetricsTest, DumpIncludesMetricsFromMultipleComponents)
{
  Metrics &metrics = Metrics::instance();

  metrics.record(Metrics::ComponentName::kExecutor,
                 "metrics_test_multi_executor", 11)();
  metrics.record(Metrics::ComponentName::kMempool, "metrics_test_multi_mempool",
                 22)();

  const std::string dump = metrics.dump_to_prometheus_string()();
  expect_dump_contains(dump, "executor_metrics_test_multi_executor 11");
  expect_dump_contains(dump, "mempool_metrics_test_multi_mempool 22");
}

TEST_F(NewMetricsTest, MetricsRouteReturnsExpectedResponse)
{
  Metrics &metrics = Metrics::instance();
  // We set the description before starting the HTTP server because accesses to
  // the [descriptions] map are not thread safe. The HTTP server reads the
  // descriptions map, so we want to avoid a race.
  metrics.set_description(Metrics::ComponentName::kExecutor,
                          "metrics_test_executor_queue_depth",
                          "The executor queue length.");

  HttpServer server;

  add_health_metrics_routes(server);
  uint16_t port = server.start_server(x23::HostPort{"127.0.0.1", 0});

  metrics.record(Metrics::ComponentName::kExecutor,
                 "metrics_test_executor_queue_depth", 17)();

  std::string http_response =
      request(port, "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(http_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(http_response.find("Content-Type: text/plain\r\n"),
            std::string::npos);
  EXPECT_NE(http_response.find(
                "# HELP executor_metrics_test_executor_queue_depth The "
                "executor queue length."),
            std::string::npos);
  EXPECT_NE(http_response.find(
                "# TYPE executor_metrics_test_executor_queue_depth gauge"),
            std::string::npos);
  EXPECT_NE(http_response.find("executor_metrics_test_executor_queue_depth 17"),
            std::string::npos);
}

} // namespace
} // namespace hogd
