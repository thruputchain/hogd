#include "hogd/hog_indexer/pull_controller.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/hog_indexer/db.h"
#include "hogd/hog_indexer/recent_tx_failure_cache.h"
#include "hogd/metrics/health.h"
#include "hogd/version.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/rpc_common.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

namespace {

template<typename T>
bool
xdr_equal(const T &lhs, const T &rhs)
{
  return x23::serialize(lhs) == x23::serialize(rhs);
}

template<typename F>
auto
call_with_timeout(event_indexer_client_t client,
                  std::chrono::milliseconds timeout, std::string_view operation,
                  F &&call)
{
  std::mutex mutex;
  std::condition_variable_any wake;
  bool completed = false;
  std::jthread deadline_guard(
      [client = std::move(client), timeout, operation, &mutex, &wake,
       &completed](const std::stop_token &stop) mutable {
        std::unique_lock lock(mutex);
        if (!wake.wait_for(lock, stop, timeout, [&] { return completed; }) &&
            !stop.stop_requested()) {
          warn("{} RPC timed out", operation);
          client.__send_call.get()->fail();
        }
      });
  x23::Defer complete([&]() noexcept {
    {
      std::lock_guard lock(mutex);
      completed = true;
    }
    wake.notify_all();
  });
  return std::forward<F>(call)();
}

} // namespace

IndexerPullController::IndexerPullController(
    std::string event_server_addr, IndexDB &db,
    RecentTxFailureCache &failure_cache, IndexerPullControllerOptions options)
  : event_server_addr_(std::move(event_server_addr)), db_(db),
    failure_cache_(failure_cache), options_(options)
{
  mark_not_ready();
  thread_ = std::jthread([this](const std::stop_token &stop) { run(stop); });
}

IndexerPullController::~IndexerPullController()
{
  stopping_.store(true, std::memory_order_release);
  fail_event_client();
  thread_.request_stop();
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void
IndexerPullController::wait_until_caught_up()
{
  std::unique_lock lock(wait_mutex_);
  wake_.wait(lock, [this] { return caught_up_; });
}

bool
IndexerPullController::wait_until_caught_up_for(
    std::chrono::milliseconds timeout)
{
  std::unique_lock lock(wait_mutex_);
  return wake_.wait_for(lock, timeout, [this] { return caught_up_; });
}

bool
IndexerPullController::connect_once()
{
  x23::HostPort event_address;
  if (!event_address.parse(event_server_addr_)) {
    panic("invalid event server address: {}", event_server_addr_);
  }
  auto resolved = event_address.to_addr(/*dns=*/true);
  if (!resolved) {
    warn("failed to resolve event server {}: {}", event_server_addr_,
         resolved.error().error_code().message());
    return false;
  }
  auto client = make_rpc_client<EventIndexer1>(*resolved, /*retry=*/false,
                                               kIndexerRpcMaxMessageSize)();
  if (!client) {
    return false;
  }
  std::lock_guard lock(event_client_mutex_);
  if (stopping_.load(std::memory_order_acquire)) {
    return false;
  }
  event_client_.emplace(std::move(*client));
  return true;
}

bool
IndexerPullController::initialize_origin_if_needed(
    const IndexerOutboxOrigin &origin)
{
  const IndexerProgress before = db_.durable_progress();
  if (before.state() == IndexerProgressState::IndexerProgressInitialized &&
      xdr_equal(before.initialized().origin, origin)) {
    return false;
  }

  const IndexerEpochResult initialized = db_.initialize_epoch(origin);
  if (initialized.outcome() != IndexerEpochOutcome::IndexerEpochInitialized &&
      initialized.outcome() != IndexerEpochOutcome::IndexerEpochUnchanged) {
    panic("event supplied an inadmissible indexer origin (outcome {}): {}",
          initialized.outcome(), initialized.diagnostic());
  }
  const IndexerProgress after = db_.durable_progress();
  if (after.state() != IndexerProgressState::IndexerProgressInitialized ||
      !xdr_equal(after.initialized(), initialized.progress()) ||
      !xdr_equal(after.initialized().origin, origin)) {
    panic(
        "indexer epoch initialization returned inconsistent durable progress");
  }
  return !xdr_equal(before, after);
}

bool
IndexerPullController::pull_tx_failures(event_indexer_client_t &client)
{
  try {
    IndexerTxFailureNoticeBatch notices =
        call_with_timeout(client, options_.rpc_timeout,
                          "indexer tx-failure pull", [&] {
                            return client.event_indexer_pull_tx_failures()();
                          });
    const BlockHeight watermark = db_.latest_block();
    for (const IndexerTxFailureNotice &notice : notices) {
      failure_cache_.report(notice.tx_hash, notice.diagnostic, watermark);
    }
    failure_cache_.prune(watermark);
    return true;
  } catch (const x23::XdrBadInput &e) {
    panic("malformed indexer tx-failure pull reply: {}", e.what());
  } catch (const x23::XdrLengthError &e) {
    panic("malformed indexer tx-failure pull reply: {}", e.what());
  } catch (const x23::XdrException &e) {
    if (!stopping_.load(std::memory_order_acquire)) {
      warn("indexer tx-failure pull transport failed: {}", e.what());
    }
    return false;
  } catch (const std::exception &e) {
    panic("internal indexer tx-failure pull failure: {}", e.what());
  }
}

void
IndexerPullController::run(const std::stop_token &stop)
{
  while (!stop.stop_requested()) {
    std::optional<event_indexer_client_t> client;
    {
      std::lock_guard lock(event_client_mutex_);
      client = event_client_;
    }
    if (!client) {
      mark_not_ready();
      try {
        if (!connect_once()) {
          if (!wait(stop, options_.retry_delay)) {
            return;
          }
          continue;
        }
      } catch (const x23::XdrException &e) {
        if (!stopping_.load(std::memory_order_acquire)) {
          warn("indexer event connection failed: {}", e.what());
        }
        if (!wait(stop, options_.retry_delay)) {
          return;
        }
        continue;
      }
      {
        std::lock_guard lock(event_client_mutex_);
        client = event_client_;
      }
      if (!client) {
        continue;
      }
    }

    bool retry_delay = false;
    bool caught_up_delay = false;
    try {
      const IndexerProgress requested_progress = db_.durable_progress();
      EventIndexerPullRequest request{
          .protocol = version::kHogdProtocol(),
          .progress = requested_progress,
      };
      EventIndexerPullResult result =
          call_with_timeout(*client, options_.rpc_timeout, "indexer block pull",
                            [&] {
                              return client->event_indexer_pull_next(request)();
                            });
      if (stop.stop_requested()) {
        return;
      }

      switch (result.outcome()) {
      case EventIndexerPullOutcome::EventIndexerPullBlock:
      {
        mark_not_ready();
        EventIndexerPulledBlock pulled = std::move(result.pulled_block());
        initialize_origin_if_needed(pulled.origin);
        const IndexerProgress before_apply = db_.durable_progress();
        const BlockHeight pulled_height =
            pulled.block.payload.consensus_value.proposal_header.block_height;
        const IndexerCommittedBlockIdentity pulled_identity =
            pulled.block.identity;
        IndexerBlockApplyResult applied =
            db_.apply_committed_block(pulled.origin, std::move(pulled.block))();
        if (applied.outcome() !=
                IndexerBlockApplyOutcome::IndexerBlockApplied &&
            applied.outcome() !=
                IndexerBlockApplyOutcome::IndexerBlockDuplicate) {
          panic("event block could not be durably applied (outcome {})",
                applied.outcome());
        }
        const IndexerProgress durable = db_.durable_progress();
        if (durable.state() !=
                IndexerProgressState::IndexerProgressInitialized ||
            !xdr_equal(durable.initialized(), applied.progress()) ||
            !xdr_equal(applied.progress().origin, pulled.origin) ||
            applied.progress().contiguous_height != pulled_height ||
            applied.progress().last_consensus_hash !=
                pulled_identity.consensus_hash ||
            applied.progress().last_payload_digest !=
                pulled_identity.payload_digest) {
          panic("applied event block disagrees with durable indexer progress");
        }
        const bool advanced =
            before_apply.state() !=
                IndexerProgressState::IndexerProgressInitialized ||
            before_apply.initialized().contiguous_height !=
                applied.progress().contiguous_height;
        if (advanced) {
          failure_cache_.prune(applied.progress().contiguous_height);
          Health::instance().set_current_block_height(
              applied.progress().contiguous_height)();
        }
        break;
      }
      case EventIndexerPullOutcome::EventIndexerPullCaughtUp:
      {
        const bool progress_changed =
            initialize_origin_if_needed(result.origin());
        const IndexerProgress durable = db_.durable_progress();
        if (progress_changed) {
          mark_not_ready();
          break;
        }
        if (!xdr_equal(durable, requested_progress) ||
            durable.state() !=
                IndexerProgressState::IndexerProgressInitialized ||
            !xdr_equal(durable.initialized().origin, result.origin())) {
          panic("caught-up reply disagrees with current durable progress");
        }
        mark_caught_up();
        caught_up_delay = true;
        break;
      }
      case EventIndexerPullOutcome::EventIndexerPullNotReady:
        mark_not_ready();
        retry_delay = true;
        break;
      case EventIndexerPullOutcome::EventIndexerPullMismatchedProtocol:
      case EventIndexerPullOutcome::EventIndexerPullProgressConflict:
      case EventIndexerPullOutcome::EventIndexerPullInternalFailure:
        panic("fatal indexer pull outcome {}: {}", result.outcome(),
              result.diagnostic());
      }

      if (!pull_tx_failures(*client)) {
        mark_not_ready();
        reset_event_client();
        retry_delay = true;
      }
    } catch (const x23::XdrCallFailed &e) {
      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }
      panic("event rejected indexer pull RPC: {}", e.what());
    } catch (const x23::XdrBadInput &e) {
      panic("malformed indexer pull reply: {}", e.what());
    } catch (const x23::XdrLengthError &e) {
      panic("malformed indexer pull reply: {}", e.what());
    } catch (const x23::XdrException &e) {
      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }
      warn("indexer pull transport failed: {}", e.what());
      mark_not_ready();
      reset_event_client();
      retry_delay = true;
    } catch (const std::exception &e) {
      panic("internal indexer pull failure: {}", e.what());
    }

    const auto delay =
        caught_up_delay ? options_.caught_up_poll_delay : options_.retry_delay;
    if ((retry_delay || caught_up_delay) && !wait(stop, delay)) {
      return;
    }
  }
}

void
IndexerPullController::mark_caught_up()
{
  {
    std::lock_guard lock(wait_mutex_);
    caught_up_ = true;
    ever_caught_up_ = true;
  }
  Health::instance().set_status(Health::Status::kOk)();
  wake_.notify_all();
}

void
IndexerPullController::mark_not_ready()
{
  Health::Status status;
  {
    std::lock_guard lock(wait_mutex_);
    caught_up_ = false;
    status =
        ever_caught_up_ ? Health::Status::kError : Health::Status::kStarting;
  }
  Health::instance().set_status(status)();
}

void
IndexerPullController::fail_event_client()
{
  std::optional<event_indexer_client_t> client;
  {
    std::lock_guard lock(event_client_mutex_);
    client = event_client_;
  }
  if (client) {
    client->__send_call.get()->fail();
  }
}

void
IndexerPullController::reset_event_client()
{
  std::lock_guard lock(event_client_mutex_);
  event_client_.reset();
}

bool
IndexerPullController::wait(std::stop_token stop,
                            std::chrono::milliseconds delay)
{
  std::unique_lock lock(wait_mutex_);
  return !wake_.wait_for(lock, std::move(stop), delay, [] { return false; });
}

} // namespace hogd
