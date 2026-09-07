#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "x23/fastclock.h"
#include "x23/rpc.h"

namespace hogd {

// Measures how long a committed block takes to become queryable: from the
// moment [eventsched_commit_block] returns on the runner to the moment the
// indexer's serving watermark covers that height.
//
// Both timestamps are taken in this process off the same [fastclock], so the
// number carries no cross-host clock skew. The indexer is only ever asked
// "what is your watermark now" -- never "when did you apply this" -- so no
// clock but the runner's is involved, and no log shipping is needed.
//
// Sampling is poll-based, so a reported latency is the true latency plus up
// to one poll interval; [kPollInterval] keeps that error small against
// anything the ingest path can produce.
//
// The poller is a plain thread making blocking RPC calls, paced by a
// condition variable so a stop request is prompt.
class IndexerLatencyProbe {
public:
  // ~1k RPCs/s against the indexer under test; tighter starts to perturb it.
  static constexpr auto kPollInterval = std::chrono::milliseconds(1);

  // How long the indexer gets to start serving queries before the bench
  // gives up on it, and how often it is retried in the meantime.
  static constexpr auto kConnectTimeout = std::chrono::seconds(120);
  static constexpr auto kConnectRetryInterval = std::chrono::seconds(1);

  // Fails loudly: a probe that silently measured nothing would publish an
  // empty metric that reads exactly like "latency is fine".
  explicit IndexerLatencyProbe(const std::string &indexer_addr)
    : client_(connect(indexer_addr))
  {
    const IndexerBlockExplorerIntroOutcome intro =
        client_.indexer_block_explorer_intro(version::kHogdProtocol())();
    if (intro != IndexerBlockExplorerIntroOutcome::IntroSuccess) {
      panic("indexer latency probe: intro rejected ({})", intro);
    }
    poller_ = std::jthread([this](const std::stop_token &stop) { poll(stop); });
  }

  ~IndexerLatencyProbe() { stop(); }

  // Disable copy and move.
  IndexerLatencyProbe(const IndexerLatencyProbe &) = delete;
  IndexerLatencyProbe &operator=(const IndexerLatencyProbe &) = delete;
  IndexerLatencyProbe(IndexerLatencyProbe &&) = delete;
  IndexerLatencyProbe &operator=(IndexerLatencyProbe &&) = delete;

  // Called on the runner thread once [eventsched_commit_block] has returned
  // for [height]. Heights must be recorded before the indexer can serve
  // them, which commit ordering guarantees.
  void record_commit(BlockHeight height, uint64_t commit_ns)
  {
    const std::lock_guard g(mtx_);
    pending_.emplace(height, commit_ns);
  }

  // Blocks until the indexer's watermark covers every recorded height, or
  // until [timeout] elapses. Returns false on timeout, leaving the
  // unobserved heights out of the report rather than guessing at them.
  bool drain(std::chrono::milliseconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mtx_);
    return drained_.wait_until(lock, deadline,
                               [this] { return pending_.empty(); });
  }

  void stop()
  {
    if (poller_.joinable()) {
      poller_.request_stop();
      wake_.notify_all();
      poller_.join();
    }
  }

  // One line per block plus a summary, both on a stable prefix so CI can
  // grep them out of the runner's stdout into their own metric series
  // without depending on the surrounding benchmark output.
  void report(std::string_view label)
  {
    std::vector<uint64_t> samples;
    size_t missed = 0;
    {
      const std::lock_guard g(mtx_);
      samples = observed_;
      missed = pending_.size();
      for (const auto &[height, latency_ns] : per_height_) {
        std::println("indexer_latency_block: label={} height={} latency_ns={}",
                     label, height, latency_ns);
      }
    }

    if (samples.empty()) {
      std::println("indexer_latency_summary: label={} blocks=0 unobserved={} "
                   "(no samples)",
                   label, missed);
      return;
    }

    std::ranges::sort(samples);
    std::println("indexer_latency_summary: label={} blocks={} unobserved={} "
                 "min_ns={} p50_ns={} p90_ns={} p99_ns={} max_ns={} mean_ns={}",
                 label, samples.size(), missed, samples.front(),
                 quantile(samples, 0.50), quantile(samples, 0.90),
                 quantile(samples, 0.99), samples.back(), mean(samples));
  }

private:
  using indexer_block_explorer_client_t =
      IndexerBlockExplorer1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  // Connects in the member init list so [client_] is never an empty optional
  // that every later use has to re-check.
  //
  // Bounded, unlike [make_rpc_client]'s own retry, which loops on a refused
  // port for as long as the process lives: an indexer that died at startup
  // would otherwise hold the whole benchmark here until the CI job's own
  // timeout kills it, an hour later, with nothing in the log but the
  // connect retry (#3890). The window covers the indexer's startup replay,
  // since it opens this port only once its pull has caught up.
  static indexer_block_explorer_client_t connect(const std::string &addr)
  {
    const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
    auto server = server_address_to_sockaddr(addr);
    while (true) {
      auto client_res =
          make_rpc_client<IndexerBlockExplorer1>(server, /*retry=*/false)();
      if (client_res) {
        return std::move(*client_res);
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        panic("indexer latency probe: connect to {} failed after {}s: {}", addr,
              kConnectTimeout.count(),
              client_res.error().error_code().message());
      }
      std::this_thread::sleep_for(kConnectRetryInterval);
    }
  }

  // Nearest-rank: the ceil(q * N)th smallest sample, 1-indexed, so every
  // reported value is one that actually occurred rather than an interpolation
  // between two blocks. For N = 10, p90 is the 9th sample, not the maximum.
  // Callers guarantee [sorted] is non-empty.
  static uint64_t quantile(const std::vector<uint64_t> &sorted, double q)
  {
    const auto rank =
        static_cast<size_t>(std::ceil(q * static_cast<double>(sorted.size())));
    return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
  }

  static uint64_t mean(const std::vector<uint64_t> &samples)
  {
    // Accumulate in double: the sum of a long run of nanosecond samples is
    // nowhere near overflowing, but the division is the only thing wanted.
    double total = 0;
    for (uint64_t sample : samples) {
      total += static_cast<double>(sample);
    }
    return static_cast<uint64_t>(total / static_cast<double>(samples.size()));
  }

  void poll(const std::stop_token &stop)
  {
    while (!stop.stop_requested()) {
      const BlockHeight watermark = read_watermark();
      const uint64_t now = x23::fastclock::get_ns();
      {
        const std::lock_guard g(mtx_);
        // [pending_] is ordered, so this walks exactly the heights this
        // sample newly covered and stops at the first one it did not.
        for (auto it = pending_.begin();
             it != pending_.end() && it->first <= watermark;) {
          const uint64_t latency_ns = now - it->second;
          observed_.push_back(latency_ns);
          per_height_.emplace(it->first, latency_ns);
          it = pending_.erase(it);
        }
        if (pending_.empty()) {
          drained_.notify_all();
        }
      }

      std::unique_lock lock(wait_mtx_);
      wake_.wait_for(lock, stop, kPollInterval, [] { return false; });
    }
  }

  // A watermark read, not a page read: the indexer's
  // [get_transactions_by_block] reports its serving bounds and returns no
  // refs, so height and limit here only have to be valid, not meaningful.
  BlockHeight read_watermark()
  {
    BlockTransactionQuery query{};
    query.height = 0;
    query.order = IterationOrder::IterationOrderAscending;
    query.limit = 1;
    try {
      const TransactionRefPageResult result =
          client_.get_transactions_by_block(query)();
      if (result.outcome() != IndexerQueryOutcome::IndexerQueryOk) {
        // Expected while the indexer is still opening its database.
        return -1;
      }
      return result.page().latest_block;
    } catch (const std::exception &e) {
      warn_every_n_sec(1.0, "indexer latency probe: watermark read failed: {}",
                       e.what());
      return -1;
    }
  }

  indexer_block_explorer_client_t client_;

  std::mutex mtx_;
  std::condition_variable drained_;
  // Committed but not yet observed as servable, oldest height first.
  std::map<BlockHeight, uint64_t> pending_;
  std::map<BlockHeight, uint64_t> per_height_;
  std::vector<uint64_t> observed_;

  std::mutex wait_mtx_;
  std::condition_variable_any wake_;
  std::jthread poller_;
};

} // namespace hogd
