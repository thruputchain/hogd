#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

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
  expect_dump_contains(dump, "# TYPE metrics_test_executor_queue_depth gauge");
  expect_dump_contains(dump, "executor_metrics_test_executor_queue_depth 17");
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
  HttpServer server;

  add_health_metrics_routes(server);
  uint16_t port = server.start_server(x23::HostPort{"127.0.0.1", 0});

  Metrics &metrics = Metrics::instance();
  metrics.record(Metrics::ComponentName::kExecutor,
                 "metrics_test_executor_queue_depth", 17)();

  std::string http_response =
      request(port, "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(http_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(http_response.find("Content-Type: text/plain\r\n"),
            std::string::npos);
  EXPECT_NE(http_response.find(
                "# TYPE metrics_test_executor_queue_depth gauge"),
            std::string::npos);
  EXPECT_NE(http_response.find("executor_metrics_test_executor_queue_depth 17"),
            std::string::npos);
}

} // namespace
} // namespace hogd
