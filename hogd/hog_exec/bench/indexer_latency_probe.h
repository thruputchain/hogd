#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
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
    if (!poller_.joinable()) {
      return;
    }
    stopping_.store(true, std::memory_order_release);
    poller_.request_stop();
    wake_.notify_all();
    // A poll blocked in an RPC on an indexer that accepted the connection and
    // then stopped answering would otherwise hold this join for as long as
    // the process lives -- the hang #3891 took out of the connect path.
    // Failing the stream makes that call throw, so the poller comes back and
    // sees the stop request; the indexer's own pull loop bounds an RPC the
    // same way (hog_indexer/pull_controller.cc).
    fail_client();
    poller_.join();
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

  // One connect-and-handshake attempt, owned by nobody. Whoever started it
  // drops its handle when it stops caring, and the attempt finishes into the
  // shared state and frees it, so an attempt that outlives its starter --
  // [tcpconnect] has no deadline of its own, so a host that black-holes SYNs
  // rather than refusing them holds one for as long as the kernel retries --
  // neither reaches freed memory nor holds up [stop].
  struct ConnectAttempt {
    std::mutex mtx;
    std::condition_variable done_cv;
    bool done = false;
    std::optional<indexer_block_explorer_client_t> client;
    std::string error;
  };

  // Connects and introduces itself on a thread of its own, reporting the
  // outcome through [attempt]. Both steps belong to the attempt: a peer that
  // accepts the connection and then never answers the handshake must be no
  // more able to hold a caller than one that never accepts it at all -- from
  // the poller that is a [stop] that cannot join (#3891), and from the
  // initial connect it is the whole benchmark stuck until the CI job's own
  // timeout (#3890).
  //
  // Touches only its arguments, so it is safe to abandon.
  static void connect_into(const std::shared_ptr<ConnectAttempt> &attempt,
                           const std::string &addr, bool retry)
  {
    std::optional<indexer_block_explorer_client_t> client;
    std::string error;
    try {
      auto client_res = make_rpc_client<IndexerBlockExplorer1>(
          server_address_to_sockaddr(addr), retry)();
      if (!client_res) {
        error = std::format("connect failed: {}",
                            client_res.error().error_code().message());
      }
      else {
        const IndexerBlockExplorerIntroOutcome intro =
            client_res->indexer_block_explorer_intro(
                version::kHogdProtocol())();
        if (intro == IndexerBlockExplorerIntroOutcome::IntroSuccess) {
          client.emplace(std::move(*client_res));
        }
        else {
          error = std::format("intro rejected ({})", intro);
        }
      }
    } catch (const std::exception &e) {
      error = std::format("handshake failed: {}", e.what());
    }

    {
      const std::lock_guard g(attempt->mtx);
      if (client.has_value()) {
        attempt->client.emplace(std::move(*client));
      }
      attempt->error = std::move(error);
      attempt->done = true;
    }
    attempt->done_cv.notify_all();
  }

  // The initial connection, made in the member init list so the probe is
  // either connected and introduced or has panicked by the time the
  // constructor returns.
  //
  // Bounded, unlike [make_rpc_client]'s own retry, which loops on a refused
  // port for as long as the process lives: an indexer that died at startup
  // would otherwise hold the whole benchmark here until the CI job's own
  // timeout kills it, an hour later, with nothing in the log but the
  // connect retry (#3890). The window is generous because the indexer opens
  // this port only once its startup replay has caught up.
  static indexer_block_explorer_client_t connect(const std::string &addr)
  {
    const std::shared_ptr<ConnectAttempt> attempt =
        std::make_shared<ConnectAttempt>();
    std::thread(connect_into, attempt, addr, /*retry=*/true).detach();

    std::unique_lock lock(attempt->mtx);
    if (!attempt->done_cv.wait_for(lock, kConnectTimeout,
                                   [&attempt] { return attempt->done; })) {
      panic("indexer latency probe: no connection to {} within {}s; the "
            "indexer is not serving queries",
            addr, kConnectTimeout.count());
    }
    if (attempt->client.has_value()) {
      return std::move(*attempt->client);
    }
    panic("indexer latency probe: {} did not accept the probe: {}", addr,
          attempt->error);
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
      // A copy, so the RPC runs without [client_mtx_] held and [stop] can
      // still reach the stream to unstick a call that is not coming back.
      std::optional<indexer_block_explorer_client_t> client = copy_client();
      if (client.has_value()) {
        const BlockHeight watermark = read_watermark(*client);
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
      // Pacing follows what the loop just did: measuring at [kPollInterval],
      // waiting for a connection at [kReconnectInterval]. [client] is this
      // tick's copy, so a read that dropped the shared one does not change
      // the pace until the next tick reads it as gone.
      wake_.wait_for(lock, stop,
                     client.has_value() ? kPollInterval : kReconnectInterval,
                     [] { return false; });
    }
  }

  // A watermark read, not a page read: the indexer's
  // [get_transactions_by_block] reports its serving bounds and returns no
  // refs, so height and limit here only have to be valid, not meaningful.
  //
  // Drops [client_] on a transport failure so the next tick reconnects
  // rather than re-asking a dead file descriptor.
  BlockHeight read_watermark(indexer_block_explorer_client_t &client)
  {
    BlockTransactionQuery query{};
    query.height = 0;
    query.order = IterationOrder::IterationOrderAscending;
    query.limit = 1;
    try {
      const TransactionRefPageResult result =
          client.get_transactions_by_block(query)();
      if (result.outcome() != IndexerQueryOutcome::IndexerQueryOk) {
        // Expected while the indexer is still opening its database. The
        // connection is fine, so this is not a failure to record.
        return -1;
      }
      return result.page().latest_block;
    } catch (const std::exception &e) {
      // [stop] fails the stream deliberately to unstick a call that is not
      // coming back, so a failure seen while stopping is this shutdown
      // rather than anything the report should blame the indexer for.
      if (!stopping_.load(std::memory_order_acquire)) {
        warn_every_n_sec(1.0,
                         "indexer latency probe: watermark read failed: {}",
                         e.what());
        note_failure(std::format("watermark read failed: {}", e.what()));
      }
      // Fail the stream before dropping it, the way the indexer's own pull
      // loop retires a broken event client: the socket is closed here rather
      // than left to the reference count, so the next poll opens a new
      // connection instead of re-asking a dead file descriptor.
      client.__send_call.get()->fail();
      drop_client();
      return -1;
    }
  }

  std::optional<indexer_block_explorer_client_t> copy_client()
  {
    const std::lock_guard g(client_mtx_);
    return client_;
  }

  void drop_client()
  {
    const std::lock_guard g(client_mtx_);
    client_.reset();
  }

  void adopt_client(indexer_block_explorer_client_t &&client)
  {
    const std::lock_guard g(client_mtx_);
    client_.emplace(std::move(client));
  }

  // Fails whatever stream the poller is on, so a call that is not coming
  // back throws instead of holding [stop].
  void fail_client()
  {
    std::optional<indexer_block_explorer_client_t> client = copy_client();
    if (client.has_value()) {
      client->__send_call.get()->fail();
    }
  }

  // Starts a reconnect attempt, or adopts the one that finished. Blocking
  // work all happens on the attempt's own thread, so this never holds the
  // poller; and at most one attempt is in flight, so a run against a host
  // that never comes back spends one thread at a time rather than one per
  // tick. Retry is off there: a single attempt reports a refused port
  // promptly, which is what lets the poller pace the retries itself.
  void service_reconnect()
  {
    if (!attempt_) {
      attempt_ = std::make_shared<ConnectAttempt>();
      std::thread(connect_into, attempt_, indexer_addr_, /*retry=*/false)
          .detach();
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
    if (!attempt->client.has_value()) {
      note_failure(
          std::format("reconnect to {}: {}", indexer_addr_, attempt->error));
      return;
    }

    adopt_client(std::move(*attempt->client));
    {
      const std::lock_guard g(mtx_);
      ++reconnects_;
    }
    info("indexer latency probe: reconnected to {}", indexer_addr_);
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
  // Set before the poller is asked to stop, so a stream this class failed on
  // purpose is not reported as the indexer's fault.
  std::atomic<bool> stopping_ = false;
  // Guards [client_], which the poller replaces and [stop] reaches into.
  // Only ever held across a copy, an emplace or a reset -- never across an
  // RPC, which is the whole point of copying it out to poll.
  std::mutex client_mtx_;
  std::optional<indexer_block_explorer_client_t> client_;
  // Poller-owned: the reconnect in flight, if any.
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
