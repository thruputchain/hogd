#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string_view>

#include "hogd/metrics/metrics.h"

namespace hogd {

// Diagnostic-only wall time from request serialization through reply decode.
// Stop before the executor's worker-pool hop: its queue is not database time.
// Samples from concurrent calls overlap and must not be summed as wall time.
class DbRpcProfile {
public:
  enum class Method {
    kGetSnapshotItem,
    kGetContract,
    kGetDeployment,
    kGetNextRetrocausalQueueBlock,
    kGetConcurrentRetrocausalQueueEntry,
    kCommitTransaction,
    kAbortTransaction,
    kTryPushNewSubtransaction,
    kPopSubtransactions,
    kGetDbTip,
    kOpenViewingSnapshot,
    kCloseViewingSnapshot,
    kCount
  };

  static void initialize()
  {
    const char *value = std::getenv("HOGDB_PROFILE");
    enabled_ = value != nullptr && std::string_view(value) == "true";
    if (!enabled_) {
      return;
    }
    auto &metrics = Metrics::instance();
    for (size_t i = 0; i < names_.size(); ++i) {
      calls_[i] =
          metrics.per_thread_counter(Metrics::ComponentName::kExecutor,
                                     std::format("db_rpc_{}_calls", names_[i]),
                                     "Database RPC attempts.");
      samples_[i] = metrics.per_thread_counter(
          Metrics::ComponentName::kExecutor,
          std::format("db_rpc_{}_samples", names_[i]),
          "Completed latency samples (one per 1024 calls per worker).");
      nanoseconds_[i] = metrics.per_thread_counter(
          Metrics::ComponentName::kExecutor,
          std::format("db_rpc_{}_sample_nanoseconds", names_[i]),
          "Sum of sampled RPC elapsed nanoseconds, excluding worker-pool hop.");
    }
  }

  explicit DbRpcProfile(Method method) : index_(static_cast<size_t>(method))
  {
    if (!enabled_) {
      return;
    }
    calls_[index_].increment();
    static thread_local std::array<uint64_t, kCount> calls{};
    sampled_ = (calls[index_]++ % 1024) == 0;
    if (sampled_) {
      start_ = std::chrono::steady_clock::now();
    }
  }

  void finish()
  {
    if (sampled_) {
      auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start_);
      nanoseconds_[index_].increment(static_cast<size_t>(elapsed.count()));
      samples_[index_].increment();
      sampled_ = false;
    }
  }

private:
  static constexpr size_t kCount = static_cast<size_t>(Method::kCount);
  static constexpr std::array<std::string_view, kCount> names_ = {
      "get_snapshot_item",
      "get_contract",
      "get_deployment",
      "get_next_retrocausal_queue_block",
      "get_concurrent_retrocausal_queue_entry",
      "commit_transaction",
      "abort_transaction",
      "try_push_new_subtransaction",
      "pop_subtransactions",
      "get_db_tip",
      "open_viewing_snapshot",
      "close_viewing_snapshot",
  };
  // Initialized before executor workers start; tests that do not initialize
  // executor metrics leave profiling disabled.
  inline static bool enabled_ = false;
  inline static std::array<Metrics::Counter, kCount> calls_;
  inline static std::array<Metrics::Counter, kCount> samples_;
  inline static std::array<Metrics::Counter, kCount> nanoseconds_;
  size_t index_;
  bool sampled_ = false;
  std::chrono::steady_clock::time_point start_{};
};

} // namespace hogd
