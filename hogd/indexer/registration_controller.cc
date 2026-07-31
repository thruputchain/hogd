#include "hogd/indexer/registration_controller.h"

#include <chrono>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/indexer/db.h"
#include "hogd/version.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "x23/cleanup.h"

namespace hogd {

IndexerRegistrationController::IndexerRegistrationController(
    std::string event_server_addr, std::string indexer_server_addr,
    const IndexerRegistrationLineage &lineage, IndexDB &db,
    IndexerRegistrationControllerOptions options)
  : event_server_addr_(std::move(event_server_addr)),
    indexer_server_addr_(std::move(indexer_server_addr)),
    generation_(lineage.generation),
    predecessor_generation_(lineage.predecessor_generation), db_(db),
    options_(options), thread_([this](std::stop_token stop) { run(stop); })
{}

IndexerRegistrationController::~IndexerRegistrationController()
{
  stopping_.store(true, std::memory_order_release);
  thread_.request_stop();
  wake_.notify_all();
  fail_event_client();
}

EventIndexerRegistrationOutcome
IndexerRegistrationController::register_once()
{
  using enum EventIndexerRegistrationOutcome;
  try {
    std::optional<event_indexer_client_t> event_client;
    {
      std::lock_guard lock(event_client_mutex_);
      event_client = event_client_;
    }
    if (!event_client) {
      x23::HostPort event_address;
      if (!event_address.parse(event_server_addr_)) {
        warn("invalid event server address: {}", event_server_addr_);
        return EventRegistrationConnectFailed;
      }
      auto resolved = event_address.to_addr(/*dns=*/true);
      if (!resolved) {
        warn("failed to resolve event server {}: {}", event_server_addr_,
             resolved.error().error_code().message());
        return EventRegistrationConnectFailed;
      }
      auto client = make_rpc_client<EventIndexer2>(*resolved, /*retry=*/false,
                                                   kIndexerRpcMaxMessageSize)();
      if (!client) {
        return EventRegistrationConnectFailed;
      }
      {
        std::lock_guard lock(event_client_mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
          return EventRegistrationConnectFailed;
        }
        event_client_.emplace(std::move(*client));
        event_client = event_client_;
      }
    }
    std::mutex rpc_wait_mutex;
    std::condition_variable_any rpc_wake;
    bool rpc_completed = false;
    std::optional<std::jthread> rpc_deadline_guard;
    if (registered_) {
      rpc_deadline_guard.emplace(
          [client = *event_client, timeout = options_.liveness_rpc_timeout,
           &rpc_wait_mutex, &rpc_wake,
           &rpc_completed](std::stop_token stop) mutable {
            std::unique_lock lock(rpc_wait_mutex);
            const bool completed = rpc_wake.wait_for(
                lock, stop, timeout, [&] { return rpc_completed; });
            if (!completed && !stop.stop_requested()) {
              warn("indexer liveness probe RPC timed out");
              client.__send_call.get()->fail();
            }
          });
    }
    x23::Defer complete_rpc([&]() noexcept {
      {
        std::lock_guard lock(rpc_wait_mutex);
        rpc_completed = true;
      }
      rpc_wake.notify_all();
    });
    if (registered_) {
      return event_client->event_indexer_probe(generation_)();
    }
    IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                       .generation = generation_,
                       .predecessor_generation = predecessor_generation_,
                       .address_for_event = indexer_server_addr_.c_str(),
                       .progress = db_.durable_progress()};
    return event_client->event_indexer_register(card)();
  } catch (const x23::XdrCallFailed &e) {
    if (stopping_.load(std::memory_order_acquire)) {
      return EventRegistrationConnectFailed;
    }
    panic("fatal indexer startup registration RPC failure: {}", e.what());
  } catch (const x23::XdrBadInput &e) {
    panic("fatal malformed indexer startup registration reply: {}", e.what());
  } catch (const x23::XdrLengthError &e) {
    panic("fatal malformed indexer startup registration reply: {}", e.what());
  } catch (const x23::XdrException &e) {
    warn("indexer startup registration transport failed: {}", e.what());
    std::lock_guard lock(event_client_mutex_);
    event_client_.reset();
  }
  return EventRegistrationConnectFailed;
}

void
IndexerRegistrationController::wait_until_registered()
{
  std::unique_lock lock(wait_mutex_);
  wake_.wait(lock, [this] { return registered_; });
}

void
IndexerRegistrationController::run(std::stop_token stop)
{
  using enum EventIndexerRegistrationOutcome;
  const auto deadline =
      std::chrono::steady_clock::now() + options_.startup_timeout;
  std::jthread deadline_guard([this, deadline](std::stop_token stop) {
    std::unique_lock lock(wait_mutex_);
    const bool completed =
        wake_.wait_until(lock, stop, deadline, [this] { return registered_; });
    if (!completed && !stop.stop_requested()) {
      panic("indexer startup registration timed out");
    }
  });
  while (!stop.stop_requested()) {
    const EventIndexerRegistrationOutcome outcome = register_once();
    if (stop.stop_requested()) {
      return;
    }
    if (outcome == EventRegistrationAccepted) {
      if (!registered_) {
        if (!db_.acknowledge_registration(generation_)) {
          panic("failed to durably acknowledge accepted indexer registration");
        }
        {
          std::lock_guard lock(wait_mutex_);
          registered_ = true;
        }
        wake_.notify_all();
      }
      if (!wait(stop, options_.liveness_probe_delay)) {
        return;
      }
      continue;
    }
    if (registered_) {
      panic("fatal indexer event connection failure after registration");
    }
    if (outcome != EventRegistrationConnectFailed &&
        outcome != EventRegistrationOriginUnavailable) {
      panic("fatal indexer startup registration outcome {}", outcome);
    }
    if (!wait(stop, options_.retry_delay)) {
      return;
    }
  }
}

void
IndexerRegistrationController::fail_event_client()
{
  std::optional<event_indexer_client_t> event_client;
  {
    std::lock_guard lock(event_client_mutex_);
    event_client = event_client_;
  }
  if (event_client) {
    event_client->__send_call.get()->fail();
  }
}

bool
IndexerRegistrationController::wait(std::stop_token stop,
                                    std::chrono::milliseconds delay)
{
  std::unique_lock lock(wait_mutex_);
  return !wake_.wait_for(lock, stop, delay, [] { return false; });
}

} // namespace hogd
