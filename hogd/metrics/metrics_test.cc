#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/http/http.h"
#include "hogd/http/http_test.h"
#include "hogd/metrics/metrics.h"
#include "hogd/metrics/shared.h"

namespace hogd {
namespace {

class MetricsTest : public ::testing::Test {
protected:
  void SetUp() override { Metrics::instance().clear(); }
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

Metrics::Gauge
init_gauge(Metrics &metrics, Metrics::ComponentName component_name,
           const std::string &metric_name, const std::string &description)
{
  Metrics::Gauge gauge =
      metrics.gauge(component_name, metric_name, description);
  metrics.mark_done_with_init(component_name);
  return gauge;
}

Metrics::Counter
init_per_thread_counter(Metrics &metrics, Metrics::ComponentName component_name,
                        const std::string &metric_name,
                        const std::string &description)
{
  Metrics::Counter counter =
      metrics.per_thread_counter(component_name, metric_name, description);
  metrics.mark_done_with_init(component_name);
  return counter;
}

TEST_F(MetricsTest, InstanceReturnsSingleton)
{
  EXPECT_EQ(&Metrics::instance(), &Metrics::instance());
}

TEST_F(MetricsTest, ComponentNamesFormatAsPrometheusPrefixes)
{
  EXPECT_EQ(std::format("{}", Metrics::ComponentName::kExecutor), "executor");
  EXPECT_EQ(std::format("{}", Metrics::ComponentName::kDb), "db");
  EXPECT_EQ(std::format("{}", Metrics::ComponentName::kMempool), "mempool");
}

TEST_F(MetricsTest, DumpIncludesRecordedGaugeForComponent)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Gauge queue_depth =
      init_gauge(metrics, Metrics::ComponentName::kExecutor,
                 "metrics_test_executor_queue_depth", "");
  queue_depth.record(17);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(
      dump, "# TYPE executor_metrics_test_executor_queue_depth gauge");
  expect_dump_contains(dump, "executor_metrics_test_executor_queue_depth 17");
}

TEST_F(MetricsTest, GaugeFactoryReturnsHandle)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Gauge queue_depth = metrics.gauge(Metrics::ComponentName::kExecutor,
                                             "metrics_test_factory_queue_depth",
                                             "Current executor queue depth.");
  metrics.mark_done_with_init(Metrics::ComponentName::kExecutor);

  queue_depth.record(17);

  EXPECT_EQ(queue_depth.value(), 17);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump, "# HELP executor_metrics_test_factory_queue_depth "
                             "Current executor queue depth.");
  expect_dump_contains(
      dump, "# TYPE executor_metrics_test_factory_queue_depth gauge");
  expect_dump_contains(dump, "executor_metrics_test_factory_queue_depth 17");
}

TEST_F(MetricsTest, CounterFactoryReturnsHandle)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Counter tx_count =
      metrics.counter(Metrics::ComponentName::kExecutor,
                      "metrics_test_factory_tx_count", "Transactions.");
  metrics.mark_done_with_init(Metrics::ComponentName::kExecutor);

  tx_count.increment();
  tx_count.increment(3);

  EXPECT_EQ(tx_count.value(), 4);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump,
                       "# TYPE executor_metrics_test_factory_tx_count counter");
  expect_dump_contains(dump, "executor_metrics_test_factory_tx_count 4");
}

TEST_F(MetricsTest, PerThreadCounterFactoryReturnsAggregatingHandle)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Counter tx_count =
      metrics.per_thread_counter(Metrics::ComponentName::kExecutor,
                                 "metrics_test_factory_per_thread_tx_count",
                                 "Transactions.");
  metrics.mark_done_with_init(Metrics::ComponentName::kExecutor);

  tx_count.increment(3);

  std::atomic<bool> recorded = false;
  std::atomic<bool> done = false;
  std::thread worker([&] {
    tx_count.increment(4);
    recorded.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  });
  while (!recorded.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  EXPECT_EQ(tx_count.value(), 7);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(
      dump, "# TYPE executor_metrics_test_factory_per_thread_tx_count counter");
  expect_dump_contains(dump,
                       "executor_metrics_test_factory_per_thread_tx_count 7");

  done.store(true, std::memory_order_release);
  worker.join();
}

TEST_F(MetricsTest, HistogramFactoryReturnsHandle)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Histogram latency =
      metrics.histogram(Metrics::ComponentName::kMempool,
                        "metrics_test_factory_validation_latency_ms",
                        "Validation latency in milliseconds", 0, 10, 5);
  metrics.mark_done_with_init(Metrics::ComponentName::kMempool);

  latency.record(0);
  latency.record(1);
  latency.record(3);
  latency.record(11);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(
      dump,
      "# TYPE mempool_metrics_test_factory_validation_latency_ms histogram");
  expect_dump_contains(
      dump,
      "mempool_metrics_test_factory_validation_latency_ms_bucket{le=\"0\"} 1");
  expect_dump_contains(
      dump,
      "mempool_metrics_test_factory_validation_latency_ms_bucket{le=\"2\"} 2");
  expect_dump_contains(
      dump,
      "mempool_metrics_test_factory_validation_latency_ms_bucket{le=\"4\"} 3");
  expect_dump_contains(
      dump,
      "mempool_metrics_test_factory_validation_latency_ms_bucket{le=\"+Inf\"} "
      "4");
  expect_dump_contains(
      dump, "mempool_metrics_test_factory_validation_latency_ms_count 4");
}

TEST_F(MetricsTest, DumpIncludesDescriptionForDescribedMetric)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Gauge mempool_depth =
      init_gauge(metrics, Metrics::ComponentName::kMempool,
                 "metrics_test_described_mempool_depth",
                 "Number of transactions waiting in the mempool");
  mempool_depth.record(42);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump,
                       "# HELP mempool_metrics_test_described_mempool_depth "
                       "Number of transactions "
                       "waiting in the mempool");
  expect_dump_contains(
      dump, "# TYPE mempool_metrics_test_described_mempool_depth gauge");
  expect_dump_contains(dump, "mempool_metrics_test_described_mempool_depth 42");
}

TEST_F(MetricsTest, DumpOmitsMissingDescription)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Gauge snapshot_count =
      init_gauge(metrics, Metrics::ComponentName::kDb,
                 "metrics_test_unrecorded_db_snapshot_count", "");
  snapshot_count.record(5);

  const std::string dump = metrics.dump_to_prometheus_string();
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

TEST_F(MetricsTest, InitializedMetricStartsAtZero)
{
  Metrics &metrics = Metrics::instance();

  init_gauge(metrics, Metrics::ComponentName::kDb,
             "metrics_test_unrecorded_db_snapshot_count",
             "Snapshots known to the database");

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump,
                       "# HELP db_metrics_test_unrecorded_db_snapshot_count "
                       "Snapshots known to the database");
  expect_dump_contains(
      dump, "# TYPE db_metrics_test_unrecorded_db_snapshot_count gauge");
  expect_dump_contains(dump, "db_metrics_test_unrecorded_db_snapshot_count 0");
}

TEST_F(MetricsTest, RecordingSameMetricUpdatesLatestSample)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Gauge snapshot_count =
      init_gauge(metrics, Metrics::ComponentName::kDb,
                 "metrics_test_db_snapshot_count", "");
  snapshot_count.record(3);
  snapshot_count.record(9);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump, "db_metrics_test_db_snapshot_count 9");

  const std::vector<std::string_view> dump_lines = get_lines(dump);
  EXPECT_FALSE(
      std::ranges::contains(dump_lines, "db_metrics_test_db_snapshot_count 3"))
      << dump;
}

TEST_F(MetricsTest, CounterIncrementUpdatesMetric)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Counter snapshot_count =
      metrics.counter(Metrics::ComponentName::kDb,
                      "metrics_test_db_added_snapshot_count", "");
  metrics.mark_done_with_init(Metrics::ComponentName::kDb);
  snapshot_count.increment(3);
  snapshot_count.increment(4);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump, "db_metrics_test_db_added_snapshot_count 7");
}

TEST_F(MetricsTest, GaugeSamplesSupportSignedValues)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Gauge snapshot_delta =
      metrics.gauge(Metrics::ComponentName::kDb,
                    "metrics_test_signed_snapshot_delta", "");
  metrics.mark_done_with_init(Metrics::ComponentName::kDb);
  snapshot_delta.record(-7);

  EXPECT_EQ(snapshot_delta.value(), -7);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump, "db_metrics_test_signed_snapshot_delta -7");
}

TEST_F(MetricsTest, DumpIncludesMetricsFromMultipleComponents)
{
  Metrics &metrics = Metrics::instance();

  Metrics::Gauge executor_metric =
      metrics.gauge(Metrics::ComponentName::kExecutor,
                    "metrics_test_multi_executor", "");
  metrics.mark_done_with_init(Metrics::ComponentName::kExecutor);
  Metrics::Gauge mempool_metric =
      metrics.gauge(Metrics::ComponentName::kMempool,
                    "metrics_test_multi_mempool", "");
  metrics.mark_done_with_init(Metrics::ComponentName::kMempool);
  executor_metric.record(11);
  mempool_metric.record(22);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump, "executor_metrics_test_multi_executor 11");
  expect_dump_contains(dump, "mempool_metrics_test_multi_mempool 22");
}

TEST_F(MetricsTest, PerThreadMetricDumpsAggregatedSamples)
{
  Metrics &metrics = Metrics::instance();
  Metrics::Counter per_thread =
      init_per_thread_counter(metrics, Metrics::ComponentName::kExecutor,
                              "metrics_test_per_thread", "");

  per_thread.increment(3);

  std::atomic<bool> recorded = false;
  std::atomic<bool> done = false;
  std::thread worker([&] {
    per_thread.increment(4);
    recorded.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  });
  while (!recorded.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump, "executor_metrics_test_per_thread 7");

  done.store(true, std::memory_order_release);
  worker.join();
}

TEST_F(MetricsTest, HistogramDumpsBuckets)
{
  Metrics &metrics = Metrics::instance();
  Metrics::Histogram latency =
      metrics.histogram(Metrics::ComponentName::kMempool,
                        "metrics_test_validation_latency_ms",
                        "Validation latency in milliseconds", 0, 10, 5);
  metrics.mark_done_with_init(Metrics::ComponentName::kMempool);

  latency.record(0);
  latency.record(1);
  latency.record(3);
  latency.record(11);

  const std::string dump = metrics.dump_to_prometheus_string();
  expect_dump_contains(dump,
                       "# HELP mempool_metrics_test_validation_latency_ms "
                       "Validation latency in milliseconds");
  expect_dump_contains(
      dump, "# TYPE mempool_metrics_test_validation_latency_ms histogram");
  expect_dump_contains(
      dump, "mempool_metrics_test_validation_latency_ms_bucket{le=\"0\"} 1");
  expect_dump_contains(
      dump, "mempool_metrics_test_validation_latency_ms_bucket{le=\"2\"} 2");
  expect_dump_contains(
      dump, "mempool_metrics_test_validation_latency_ms_bucket{le=\"4\"} 3");
  expect_dump_contains(
      dump, "mempool_metrics_test_validation_latency_ms_bucket{le=\"+Inf\"} 4");
  expect_dump_contains(dump,
                       "mempool_metrics_test_validation_latency_ms_count 4");
}

TEST_F(MetricsTest, MetricsRouteGadgetReturnsExpectedResponse)
{
  Metrics &metrics = Metrics::instance();
  // We set the description before starting the HTTP server because accesses to
  // the [descriptions] map are not thread safe. The HTTP server reads the
  // descriptions map, so we want to avoid a race.
  Metrics::Gauge queue_depth =
      init_gauge(metrics, Metrics::ComponentName::kExecutor,
                 "metrics_test_executor_queue_depth",
                 "The executor queue length.");

  HttpServer server;

  add_health_metrics_routes(server);
  uint16_t port = server.start_server(x23::HostPort{"127.0.0.1", 0});

  queue_depth.record(17);

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

TEST_F(MetricsTest, MetricsRouteHistogramReturnsExpectedResponse)
{
  Metrics &metrics = Metrics::instance();
  Metrics::Histogram latency =
      metrics.histogram(Metrics::ComponentName::kMempool,
                        "metrics_test_validation_latency_ms",
                        "Validation latency in milliseconds", 0, 10, 5);
  metrics.mark_done_with_init(Metrics::ComponentName::kMempool);

  HttpServer server;

  add_health_metrics_routes(server);
  uint16_t port = server.start_server(x23::HostPort{"127.0.0.1", 0});

  latency.record(0);
  latency.record(1);
  latency.record(3);
  latency.record(11);

  std::string http_response =
      request(port, "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");

  expect_dump_contains(http_response,
                       "# HELP mempool_metrics_test_validation_latency_ms "
                       "Validation latency in milliseconds");
  expect_dump_contains(
      http_response,
      "# TYPE mempool_metrics_test_validation_latency_ms histogram");
  expect_dump_contains(
      http_response,
      "mempool_metrics_test_validation_latency_ms_bucket{le=\"0\"} 1");
  expect_dump_contains(
      http_response,
      "mempool_metrics_test_validation_latency_ms_bucket{le=\"2\"} 2");
  expect_dump_contains(
      http_response,
      "mempool_metrics_test_validation_latency_ms_bucket{le=\"4\"} 3");
  expect_dump_contains(
      http_response,
      "mempool_metrics_test_validation_latency_ms_bucket{le=\"+Inf\"} 4");
  expect_dump_contains(http_response,
                       "mempool_metrics_test_validation_latency_ms_count 4");
}

} // namespace
} // namespace hogd
