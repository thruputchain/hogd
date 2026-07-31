#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/indexer/shared.h"
#include "indexer.hh"

namespace hogd {

class IndexDB;
struct IndexerRegistrationLineage;

struct IndexerRegistrationControllerOptions {
  std::chrono::milliseconds startup_timeout = std::chrono::seconds(30);
  std::chrono::milliseconds retry_delay = std::chrono::milliseconds(100);
  std::chrono::milliseconds liveness_probe_delay = std::chrono::seconds(1);
  std::chrono::milliseconds liveness_rpc_timeout = std::chrono::seconds(30);
};

// Registers the standalone indexer during startup. Once replay and activation
// succeed, losing either peer is a process-level failure handled by the RPC
// transport rather than a hot-replacement workflow.
class IndexerRegistrationController {
public:
  IndexerRegistrationController(
      std::string event_server_addr, std::string indexer_server_addr,
      const IndexerRegistrationLineage &lineage, IndexDB &db,
      IndexerRegistrationControllerOptions options = {});
  ~IndexerRegistrationController();

  IndexerRegistrationController(const IndexerRegistrationController &) = delete;
  IndexerRegistrationController &
  operator=(const IndexerRegistrationController &) = delete;

  EventIndexerRegistrationOutcome register_once();
  void wait_until_registered();
  void disconnect_event_for_test() { fail_event_client(); }

private:
  void run(std::stop_token stop);
  bool wait(std::stop_token stop, std::chrono::milliseconds delay);
  void fail_event_client();

  std::string event_server_addr_;
  std::string indexer_server_addr_;
  UPID generation_;
  x23::Ptr<UPID> predecessor_generation_;
  IndexDB &db_;
  IndexerRegistrationControllerOptions options_;
  std::mutex event_client_mutex_;
  std::optional<event_indexer_client_t> event_client_;
  std::mutex wait_mutex_;
  std::condition_variable_any wake_;
  bool registered_ = false;
  std::atomic<bool> stopping_ = false;
  std::jthread thread_;
};

} // namespace hogd
