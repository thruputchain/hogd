#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "x23/fastclock.h"

namespace hogd {

// DispatchPool routes job submissions across multiple WorkerPools based on
// CPU LLC (Last-Level Cache) affinity. This reduces contention on the
// single mutex-protected queue in WorkerPool by distributing submissions
// across kBucketCount separate pools.
//
// Each WorkerPool has nthreads/kBucketCount threads, where nthreads is the
// total number of worker threads requested.
class DispatchPool {
public:
  static constexpr int kBucketCount = 128;
  // Create a DispatchPool with nthreads total worker threads distributed
  // across kBucketCount WorkerPools.
  explicit DispatchPool(int nthreads)
    : pools_(std::min<int>(kBucketCount, nthreads)), job_counts_(pools_.size()),
      threads_per_pool_(divide_up<size_t>(nthreads, pools_.size()))
  {
    int bucket_count = pools_.size();

    for (int i = 0; i < bucket_count; ++i) {
      pools_[i] = std::make_unique<WorkerPool>(threads_per_pool_);
      job_counts_[i].store(0, std::memory_order_relaxed);
    }
  }

  // Disable copy and move
  DispatchPool(DispatchPool const &) = delete;
  DispatchPool &operator=(DispatchPool const &) = delete;
  DispatchPool(DispatchPool &&) = delete;
  DispatchPool &operator=(DispatchPool &&) = delete;

  // worker_count returns the total number of available workers across all
  // pools.
  size_t worker_count() const
  {
    size_t total = 0;
    for (const auto &pool : pools_) {
      total += pool->worker_count();
    }
    return total;
  }

  // submit enqueues a fire-and-forget job to a WorkerPool, selecting between
  // two random pools based on pending job counts (power-of-two-choices).
  template<typename Func> requires std::is_invocable_r_v<void, Func>
  void submit(Func &&func)
  {
    assert(!pools_.empty());

    // Only relevant for stress tests where DEFAULT_THREADPOOL_SIZE == 1
    if (pools_.size() == 1) [[unlikely]] {
      job_counts_[0].fetch_add(1, std::memory_order_relaxed);
      pools_[0]->submit_detached(std::forward<Func>(func));
      return;
    }
    // Pick a random other pool to compare (excluding affinity_pool)
    static thread_local uint64_t rnd_seed{
        (std::hash<std::thread::id>{}(std::this_thread::get_id()) &
         0xffffffff) |
        1UL << 20 | x23::fastclock::get_ns() << 32};

    // Simple xorshift RNG
    auto rnd = [](uint64_t &seed) noexcept -> uint64_t {
      uint64_t x = seed;
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      return seed = x;
    };

    int r1 = rnd(rnd_seed) % pools_.size();
    size_t pending_1 = pools_[r1]->pending_jobs();

    // Fast path: if the first pool has idle workers, skip the second lookup.
    // This avoids an RNG call, a modulo, and an atomic load on a random
    // cache line — saving ~15-30ns per submit under light-to-moderate load.
    int pool_idx;
    if (pending_1 < threads_per_pool_) {
      pool_idx = r1;
    }
    else {
      // Power-of-two-choices: pick a second random pool and use the
      // less-loaded one.
      int r2 = rnd(rnd_seed) % (pools_.size() - 1);
      if (r2 >= r1) {
        r2 = r2 + 1;
      }
      size_t pending_2 = pools_[r2]->pending_jobs();
      pool_idx = (pending_2 < pending_1) ? r2 : r1;
    }

    // Increment counters
    job_counts_[pool_idx].fetch_add(1, std::memory_order_relaxed);

    pools_[pool_idx]->submit_detached(std::forward<Func>(func));
  }

  // print_stats prints per-pool job counts and resets them.
  void print_stats()
  {
    std::vector<size_t> counts(pools_.size());
    std::vector<size_t> pending(pools_.size());
    size_t total = 0;
    size_t total_pending = 0;
    for (auto i = 0U; i < pools_.size(); ++i) {
      counts[i] = job_counts_[i].load(std::memory_order_relaxed);
      pending[i] = pools_[i]->pending_jobs();
      total += counts[i];
      total_pending += pending[i];
    }
    if (total == 0 && total_pending == 0) {
      return;
    }
    info("DispatchPool stats: total={}, pending={}", total, total_pending);
    for (auto i = 0U; i < pools_.size(); ++i) {
      double pct = total > 0 ? (100.0 * counts[i] / total) : 0.0;
      info("  pool[{}]: {} jobs ({:.1f}%), {} pending", i, counts[i], pct,
           pending[i]);
    }
    // Reset counters after printing
    for (auto i = 0U; i < pools_.size(); ++i) {
      job_counts_[i].store(0, std::memory_order_relaxed);
    }
  }

private:
  std::vector<std::unique_ptr<WorkerPool>> pools_;
  std::vector<std::atomic<size_t>> job_counts_;
  size_t threads_per_pool_;
};

} // namespace hogd
