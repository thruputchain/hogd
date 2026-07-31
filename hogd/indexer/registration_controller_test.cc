#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/registration_controller.h"
#include "indexer.hh"
#include "network.hh"
#include "x23/async.h"

namespace hogd {
namespace {

struct AcceptingRegistrationRpc {
  using rpc_service = EventIndexer2;

  static x23::Async<EventIndexerRegistrationOutcome>
  event_indexer_register(IndexerCardV2 /*unused*/)
  {
    co_return EventIndexerRegistrationOutcome::EventRegistrationAccepted;
  }

  static x23::Async<EventIndexerRegistrationOutcome>
  event_indexer_probe(UPID /*unused*/)
  {
    co_return EventIndexerRegistrationOutcome::EventRegistrationAccepted;
  }
};

static_assert(is_EventIndexer2<AcceptingRegistrationRpc *, x23::MaybeAsync>);

struct BlockingRegistrationRpc {
  using rpc_service = EventIndexer2;

  static x23::Async<EventIndexerRegistrationOutcome>
  event_indexer_register(IndexerCardV2 /*unused*/)
  {
    std::promise<void> never;
    never.get_future().wait();
    co_return EventIndexerRegistrationOutcome::EventRegistrationAccepted;
  }

  static x23::Async<EventIndexerRegistrationOutcome>
  event_indexer_probe(UPID /*unused*/)
  {
    co_return EventIndexerRegistrationOutcome::EventRegistrationAccepted;
  }
};

static_assert(is_EventIndexer2<BlockingRegistrationRpc *, x23::MaybeAsync>);

struct StallingLivenessProbeRpc {
  using rpc_service = EventIndexer2;

  static x23::Async<EventIndexerRegistrationOutcome>
  event_indexer_register(IndexerCardV2 /*unused*/)
  {
    co_return EventIndexerRegistrationOutcome::EventRegistrationAccepted;
  }

  static x23::Async<EventIndexerRegistrationOutcome>
  event_indexer_probe(UPID /*unused*/)
  {
    calls_.fetch_add(1, std::memory_order_relaxed);
    std::promise<void> never;
    never.get_future().wait();
    co_return EventIndexerRegistrationOutcome::EventRegistrationAccepted;
  }

  static inline std::atomic<std::size_t> calls_ = 0;
};

static_assert(is_EventIndexer2<StallingLivenessProbeRpc *, x23::MaybeAsync>);

TEST(IndexerRegistrationController, WaitsForAcceptedRegistration)
{
  TmpDir indexer_dir;
  auto db = IndexDB::make_new(std::filesystem::path(indexer_dir.directory()) /
                              "indexer-db");
  ASSERT_TRUE(db.has_value()) << db.error();
  UPID generation{};
  generation[0] = 0x41;
  auto lineage = db->prepare_registration_lineage(generation);
  ASSERT_TRUE(lineage.has_value()) << lineage.error();

  RpcServer<AcceptingRegistrationRpc> event_server;
  const std::uint16_t event_port =
      event_server.start_server(x23::HostPort{"127.0.0.1", 0});
  IndexerRegistrationController controller(
      std::string(x23::HostPort{"127.0.0.1", event_port}.show()), "127.0.0.1:1",
      *lineage, *db);

  controller.wait_until_registered();
  auto acknowledged = db->registration_lineage();
  ASSERT_TRUE(acknowledged.has_value());
  const auto *acknowledged_lineage =
      acknowledged.has_value() ? &acknowledged.value() : nullptr;
  ASSERT_NE(acknowledged_lineage, nullptr);
  EXPECT_TRUE(acknowledged_lineage->acknowledged);
}

TEST(IndexerRegistrationController, StalledRpcCannotExceedStartupDeadline)
{
  ASSERT_DEATH(
      {
        TmpDir indexer_dir;
        auto db = IndexDB::make_new(
            std::filesystem::path(indexer_dir.directory()) / "indexer-db");
        if (!db.has_value()) {
          std::abort();
        }
        UPID generation{};
        auto lineage = db->prepare_registration_lineage(generation);
        if (!lineage.has_value()) {
          std::abort();
        }
        RpcServer<BlockingRegistrationRpc> event_server;
        const std::uint16_t event_port =
            event_server.start_server(x23::HostPort{"127.0.0.1", 0});
        IndexerRegistrationControllerOptions options;
        options.startup_timeout = std::chrono::milliseconds(100);
        IndexerRegistrationController controller(
            std::string(x23::HostPort{"127.0.0.1", event_port}.show()),
            "127.0.0.1:1", *lineage, *db, options);
        controller.wait_until_registered();
      },
      "indexer startup registration timed out");
}

TEST(IndexerRegistrationController, EventDisconnectAfterRegistrationIsFatal)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  ASSERT_DEATH(
      {
        TmpDir indexer_dir;
        auto db = IndexDB::make_new(
            std::filesystem::path(indexer_dir.directory()) / "indexer-db");
        if (!db.has_value()) {
          std::abort();
        }
        UPID generation{};
        auto lineage = db->prepare_registration_lineage(generation);
        if (!lineage.has_value()) {
          std::abort();
        }
        RpcServer<AcceptingRegistrationRpc> event_server;
        const std::uint16_t event_port =
            event_server.start_server(x23::HostPort{"127.0.0.1", 0});
        IndexerRegistrationControllerOptions options;
        options.liveness_probe_delay = std::chrono::milliseconds(10);
        IndexerRegistrationController controller(
            std::string(x23::HostPort{"127.0.0.1", event_port}.show()),
            "127.0.0.1:1", *lineage, *db, options);
        controller.wait_until_registered();
        controller.disconnect_event_for_test();
        std::this_thread::sleep_for(std::chrono::seconds(2));
      },
      "fatal indexer event connection failure after registration");
}

TEST(IndexerRegistrationController, StalledLivenessRpcAfterRegistrationIsFatal)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  ASSERT_DEATH(
      {
        TmpDir indexer_dir;
        auto db = IndexDB::make_new(
            std::filesystem::path(indexer_dir.directory()) / "indexer-db");
        if (!db.has_value()) {
          std::abort();
        }
        UPID generation{};
        auto lineage = db->prepare_registration_lineage(generation);
        if (!lineage.has_value()) {
          std::abort();
        }
        StallingLivenessProbeRpc::calls_.store(0, std::memory_order_relaxed);
        RpcServer<StallingLivenessProbeRpc> event_server;
        const std::uint16_t event_port =
            event_server.start_server(x23::HostPort{"127.0.0.1", 0});
        IndexerRegistrationControllerOptions options;
        options.liveness_probe_delay = std::chrono::milliseconds(10);
        options.liveness_rpc_timeout = std::chrono::milliseconds(100);
        IndexerRegistrationController controller(
            std::string(x23::HostPort{"127.0.0.1", event_port}.show()),
            "127.0.0.1:1", *lineage, *db, options);
        controller.wait_until_registered();
        std::this_thread::sleep_for(std::chrono::seconds(2));
      },
      "fatal indexer event connection failure after registration");
}

} // namespace
} // namespace hogd
