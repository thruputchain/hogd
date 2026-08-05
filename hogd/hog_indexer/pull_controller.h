#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "hogd/hog_indexer/shared.h"

namespace hogd {

class IndexDB;
class RecentTxFailureCache;
struct IndexerOutboxOrigin;

struct IndexerPullControllerOptions {
  std::chrono::milliseconds startup_timeout = std::chrono::seconds(30);
  std::chrono::milliseconds retry_delay = std::chrono::milliseconds(100);
  std::chrono::milliseconds caught_up_poll_delay =
      std::chrono::milliseconds(100);
  std::chrono::milliseconds rpc_timeout = std::chrono::seconds(30);
};

// Owns the standalone indexer's event connection and durable replay loop.
class IndexerPullController {
public:
  IndexerPullController(std::string event_server_addr, IndexDB &db,
                        RecentTxFailureCache &failure_cache,
                        IndexerPullControllerOptions options = {});
  ~IndexerPullController();

  IndexerPullController(const IndexerPullController &) = delete;
  IndexerPullController &operator=(const IndexerPullController &) = delete;

  void wait_until_caught_up();

private:
  void run(const std::stop_token &stop);
  bool connect_once();
  bool pull_tx_failures(event_indexer_client_t &client);
  bool initialize_origin_if_needed(const IndexerOutboxOrigin &origin);
  bool wait(std::stop_token stop, std::chrono::milliseconds delay);
  void fail_event_client();
  void reset_event_client();

  std::string event_server_addr_;
  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
  IndexerPullControllerOptions options_;
  std::mutex event_client_mutex_;
  std::optional<event_indexer_client_t> event_client_;
  std::mutex wait_mutex_;
  std::condition_variable_any wake_;
  bool caught_up_ = false;
  std::atomic<bool> stopping_ = false;
  std::jthread thread_;
};

} // namespace hogd
