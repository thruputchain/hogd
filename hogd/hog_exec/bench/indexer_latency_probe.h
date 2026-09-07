#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
//
// A watermark read that fails on the transport drops the connection and the
// poller reconnects, because the alternative is what #3895 was: one dropped
// connection left every later poll failing on the same dead file descriptor
// for the rest of the run, and the probe reported "no samples" without ever
// saying that it had stopped being able to ask.
class IndexerLatencyProbe {
public:
  // ~1k RPCs/s against the indexer under test; tighter starts to perturb it.
  static constexpr auto kPollInterval = std::chrono::milliseconds(1);

  // How long the indexer gets to start serving queries before the bench
  // gives up on it.
  static constexpr auto kConnectTimeout = std::chrono::seconds(120);

  // Cadence of reconnect attempts while disconnected. Far slower than
  // [kPollInterval]: there is nothing to measure until a connection is back,
  // and an indexer that has gone away should not be hammered at 1 kHz.
  static constexpr auto kReconnectInterval = std::chrono::milliseconds(500);

  // Fails loudly: a probe that silently measured nothing would publish an
  // empty metric that reads exactly like "latency is fine".
  explicit IndexerLatencyProbe(const std::string &indexer_addr)
    : indexer_addr_(indexer_addr), client_(connect(indexer_addr))
  {
    const IndexerBlockExplorerIntroOutcome intro =
        client_->indexer_block_explorer_intro(version::kHogdProtocol())();
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

  // True when the probe measured nothing at all, which is the shape of a
  // broken run rather than a fast one and is worth diagnosing further.
  bool measured_nothing()
  {
    const std::lock_guard g(mtx_);
    return observed_.empty();
  }

  // One line per block plus a summary, both on a stable prefix so CI can
  // grep them out of the runner's stdout into their own metric series
  // without depending on the surrounding benchmark output.
  void report(std::string_view label)
  {
    std::vector<uint64_t> samples;
    size_t missed = 0;
    std::string diagnosis;
    size_t failures = 0;
    {
      const std::lock_guard g(mtx_);
      samples = observed_;
      missed = pending_.size();
      failures = failures_;
      diagnosis = describe_failures();
      for (const auto &[height, latency_ns] : per_height_) {
        std::println("indexer_latency_block: label={} height={} latency_ns={}",
                     label, height, latency_ns);
      }
    }

    if (samples.empty()) {
      // The reason rides on the summary line itself because that is the line
      // [scripts/ci/exec_scaling.sh] replays when it refuses to publish the
      // series: a bare "blocks=0" names the symptom and nothing else.
      std::println("indexer_latency_summary: label={} blocks=0 unobserved={} "
                   "(no samples; {})",
                   label, missed, diagnosis);
      return;
    }

    // Kept off the metric lines, whose format the history tooling parses, but
    // still said out loud: a run that lost the connection and got it back
    // measured a gap it cannot see in its own percentiles.
    if (failures != 0) {
      warn("indexer latency probe: label={} measured {} blocks despite {}",
           label, samples.size(), diagnosis);
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

  // One reconnect attempt, owned by nobody. The probe drops its handle on
  // stop and the attempt finishes into the shared state and frees it, so an
  // attempt that outlives the probe -- [tcpconnect] has no deadline of its
  // own, so a host that black-holes SYNs rather than refusing them holds one
  // attempt for as long as the kernel retries -- neither reaches freed
  // memory nor holds up [stop].
  struct ConnectAttempt {
    std::mutex mtx;
    bool done = false;
    std::optional<indexer_block_explorer_client_t> client;
    std::string error;
  };

  // Connects in the member init list so the probe is either connected or has
  // panicked by the time the constructor returns; only [poll] ever empties
  // [client_], and only it looks at the optional.
  //
  // Bounded, unlike [make_rpc_client]'s own retry, which loops on a refused
  // port for as long as the process lives: an indexer that died at startup
  // would otherwise hold the whole benchmark here until the CI job's own
  // timeout kills it, an hour later, with nothing in the log but the
  // connect retry (#3890). The window is generous because the indexer opens
  // this port only once its startup replay has caught up.
  //
  // Waiting on the connect from another thread is what makes the bound hold
  // in every case: [tcpconnect] itself has no deadline, so a host that
  // drops SYNs rather than refusing them -- a bench host that went away
  // mid-run -- would sit in one attempt for as long as the kernel retries.
  // The abandoned attempt outlives this call only to be torn down with the
  // process [panic] is about to end.
  static indexer_block_explorer_client_t connect(const std::string &addr)
  {
    auto pending = std::async(std::launch::async, [addr]() {
      return make_rpc_client<IndexerBlockExplorer1>(server_address_to_sockaddr(
                                                        addr),
                                                    /*retry=*/true)();
    });
    if (pending.wait_for(kConnectTimeout) != std::future_status::ready) {
      panic("indexer latency probe: no connection to {} within {}s; the "
            "indexer is not serving queries",
            addr, kConnectTimeout.count());
    }
    auto client_res = pending.get();
    if (!client_res) {
      panic("indexer latency probe: connect to {} failed: {}", addr,
            client_res.error().error_code().message());
    }
    return std::move(*client_res);
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
      const bool connected = client_.has_value();
      if (connected) {
        const BlockHeight watermark = read_watermark();
        const uint64_t now = x23::fastclock::get_ns();
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
      else {
        service_reconnect();
      }

      std::unique_lock lock(wait_mtx_);
      // Pacing follows what the loop is doing: measuring at [kPollInterval],
      // waiting for a connection at [kReconnectInterval].
      wake_.wait_for(lock, stop, connected ? kPollInterval : kReconnectInterval,
                     [] { return false; });
    }
  }

  // A watermark read, not a page read: the indexer's
  // [get_transactions_by_block] reports its serving bounds and returns no
  // refs, so height and limit here only have to be valid, not meaningful.
  //
  // Called only with [client_] engaged, and empties it on a transport
  // failure so the next tick reconnects rather than re-asking a dead fd.
  BlockHeight read_watermark()
  {
    BlockTransactionQuery query{};
    query.height = 0;
    query.order = IterationOrder::IterationOrderAscending;
    query.limit = 1;
    try {
      const TransactionRefPageResult result =
          client_->get_transactions_by_block(query)();
      if (result.outcome() != IndexerQueryOutcome::IndexerQueryOk) {
        // Expected while the indexer is still opening its database. The
        // connection is fine, so this is not a failure to record.
        return -1;
      }
      return result.page().latest_block;
    } catch (const std::exception &e) {
      warn_every_n_sec(1.0, "indexer latency probe: watermark read failed: {}",
                       e.what());
      note_failure(std::format("watermark read failed: {}", e.what()));
      // Fail the stream before dropping it, the way the indexer's own pull
      // loop retires a broken event client: the socket is closed here rather
      // than left to the reference count, so the next poll opens a new
      // connection instead of re-asking a dead file descriptor.
      client_->__send_call.get()->fail();
      client_.reset();
      return -1;
    }
  }

  // Starts a reconnect attempt, or adopts the one that finished. At most one
  // attempt is in flight, so a run against a host that never comes back
  // spends one thread at a time rather than one per tick.
  void service_reconnect()
  {
    if (!attempt_) {
      attempt_ = std::make_shared<ConnectAttempt>();
      std::thread(connect_into, attempt_, indexer_addr_).detach();
      return;
    }

    {
      const std::lock_guard g(attempt_->mtx);
      if (!attempt_->done) {
        return;
      }
    }
    // Finished, so this thread is now the only one that touches it.
    const std::shared_ptr<ConnectAttempt> attempt = std::move(attempt_);
    if (!attempt->client) {
      note_failure(std::format("reconnect to {} failed: {}", indexer_addr_,
                               attempt->error));
      return;
    }

    // The handshake belongs to the connection, not to the probe, so a new
    // connection has to introduce itself again before it is trusted.
    try {
      const IndexerBlockExplorerIntroOutcome intro =
          attempt->client->indexer_block_explorer_intro(
              version::kHogdProtocol())();
      if (intro != IndexerBlockExplorerIntroOutcome::IntroSuccess) {
        warn("indexer latency probe: reconnect intro rejected ({})", intro);
        note_failure("reconnect intro was rejected");
        return;
      }
    } catch (const std::exception &e) {
      note_failure(std::format("reconnect intro failed: {}", e.what()));
      return;
    }

    client_.emplace(std::move(*attempt->client));
    {
      const std::lock_guard g(mtx_);
      ++reconnects_;
    }
    info("indexer latency probe: reconnected to {}", indexer_addr_);
  }

  // Runs on a detached thread and touches only its own arguments. Retry is
  // off: one attempt that reports a refused port promptly is what lets the
  // poller pace the retries itself.
  static void connect_into(const std::shared_ptr<ConnectAttempt> &attempt,
                           const std::string &addr)
  {
    std::optional<indexer_block_explorer_client_t> client;
    std::string error;
    try {
      auto client_res =
          make_rpc_client<IndexerBlockExplorer1>(server_address_to_sockaddr(
                                                     addr),
                                                 /*retry=*/false)();
      if (client_res) {
        client.emplace(std::move(*client_res));
      }
      else {
        error = client_res.error().error_code().message();
      }
    } catch (const std::exception &e) {
      error = e.what();
    }

    const std::lock_guard g(attempt->mtx);
    if (client) {
      attempt->client.emplace(std::move(*client));
    }
    attempt->error = std::move(error);
    attempt->done = true;
  }

  // Records why the probe could not read a watermark. Kept to one line so it
  // can ride on the summary line CI replays.
  void note_failure(std::string what)
  {
    std::ranges::replace(what, '\n', ' ');
    const std::lock_guard g(mtx_);
    ++failures_;
    if (first_error_.empty()) {
      first_error_ = what;
    }
    last_error_ = std::move(what);
  }

  // Caller holds [mtx_].
  std::string describe_failures() const
  {
    if (failures_ == 0) {
      return "the indexer served every watermark read but never reported one "
             "covering a committed height";
    }
    if (first_error_ == last_error_) {
      return std::format("{} failed watermark reads, {} reconnects: {}",
                         failures_, reconnects_, first_error_);
    }
    return std::format("{} failed watermark reads, {} reconnects; first: {}; "
                       "last: {}",
                       failures_, reconnects_, first_error_, last_error_);
  }

  const std::string indexer_addr_;
  // Poller-owned once the constructor returns; empty only while
  // disconnected.
  std::optional<indexer_block_explorer_client_t> client_;
  std::shared_ptr<ConnectAttempt> attempt_;

  std::mutex mtx_;
  std::condition_variable drained_;
  // Committed but not yet observed as servable, oldest height first.
  std::map<BlockHeight, uint64_t> pending_;
  std::map<BlockHeight, uint64_t> per_height_;
  std::vector<uint64_t> observed_;
  // Why the probe stopped being able to ask, for the report to carry.
  size_t failures_ = 0;
  size_t reconnects_ = 0;
  std::string first_error_;
  std::string last_error_;

  std::mutex wait_mtx_;
  std::condition_variable_any wake_;
  std::jthread poller_;
};

} // namespace hogd
