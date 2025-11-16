#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>

#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "x23/cpumap.h"
#include "x23/fastclock.h"

namespace hogd {

// DispatchPool routes job submissions across multiple WorkerPools based on
// CPU LLC (Last-Level Cache) affinity. This reduces contention on the
// single mutex-protected queue in WorkerPool by distributing submissions
// across kBucketCount separate pools.
//
// Each WorkerPool has nthreads/kBucketCount threads, where nthreads is the total
// number of worker threads requested.
class DispatchPool {
public:
  static constexpr int kBucketCount = 16;
  // Create a DispatchPool with nthreads total worker threads distributed
  // across kBucketCount WorkerPools.
  explicit DispatchPool(int nthreads)
  {
    x23::init_cpumaps();
    int threads_per_pool = divide_up(nthreads, kBucketCount); // round up
    for (int i = 0; i < kBucketCount; ++i) {
      pools_[i] = std::make_unique<WorkerPool>(threads_per_pool);
      job_counts_[i].store(0, std::memory_order_relaxed);
      pending_job_counts_[i].store(0, std::memory_order_relaxed);
    }
  }

  // Disable copy and move
  DispatchPool(DispatchPool const &) = delete;
  DispatchPool &operator=(DispatchPool const &) = delete;
  DispatchPool(DispatchPool &&) = delete;
  DispatchPool &operator=(DispatchPool &&) = delete;

  // worker_count returns the total number of available workers across all pools.
  size_t worker_count() const
  {
    size_t total = 0;
    for (const auto &pool : pools_) {
      total += pool->worker_count();
    }
    return total;
  }

  // submit enqueues a job to a WorkerPool, selecting between the current CPU's
  // LLC bucket pool and a random other pool based on pending job counts.
  template<typename Func,
           typename Ret = std::invoke_result_t<std::decay_t<Func>>>
  requires std::is_invocable_r_v<Ret, Func>
  std::future<Ret> submit(Func &&func)
  {
    // Get current CPU's LLC bucket
    int bucket = x23::core_map->bucket();
    int affinity_pool = bucket % kBucketCount;
    
    // Pick a random other pool to compare (excluding affinity_pool)
    static thread_local uint64_t rnd_seed{
        (std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffffff) |
        1UL << 20 | x23::fastclock::get_ns() << 32};
    
    // Simple xorshift RNG
    auto rnd = [](uint64_t &seed) noexcept -> uint64_t {
      uint64_t x = seed;
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      return seed = x;
    };
    
    // Pick random pool excluding affinity_pool
    int r = rnd(rnd_seed) % (kBucketCount - 1); // 0-(kBucketCount-2)
    int other_pool = r + (r >= affinity_pool ? 1 : 0);
    
    // Compare pending counts and pick the pool with fewer pending jobs
    size_t affinity_pending = pending_job_counts_[affinity_pool].load(std::memory_order_relaxed);
    size_t other_pending = pending_job_counts_[other_pool].load(std::memory_order_relaxed);
    int pool_idx = (other_pending < affinity_pending) ? other_pool : affinity_pool;
    
    // Increment counters
    job_counts_[pool_idx].fetch_add(1, std::memory_order_relaxed);
    pending_job_counts_[pool_idx].fetch_add(1, std::memory_order_relaxed);
    
    // Wrap the function to decrement pending count when it completes
    auto wrapped_func = [this, pool_idx, func = std::forward<Func>(func)]() mutable -> Ret {
      // Use RAII to ensure pending count is decremented even on exception
      struct DecrementGuard {
        std::atomic<size_t> *counter;
        ~DecrementGuard() { counter->fetch_sub(1, std::memory_order_relaxed); }
      } guard{&pending_job_counts_[pool_idx]};
      
      if constexpr (std::is_void_v<Ret>) {
        func();
      } else {
        return func();
      }
    };
    
    return pools_[pool_idx]->submit(std::move(wrapped_func));
  }

  // print_stats prints per-pool job counts and resets them.
  void print_stats()
  {
    std::array<size_t, kBucketCount> counts;
    std::array<size_t, kBucketCount> pending;
    size_t total = 0;
    size_t total_pending = 0;
    for (int i = 0; i < kBucketCount; ++i) {
      counts[i] = job_counts_[i].load(std::memory_order_relaxed);
      pending[i] = pending_job_counts_[i].load(std::memory_order_relaxed);
      total += counts[i];
      total_pending += pending[i];
    }
    if (total == 0 && total_pending == 0) {
      return;
    }
    info("DispatchPool stats: total={}, pending={}", total, total_pending);
    for (int i = 0; i < kBucketCount; ++i) {
      double pct = total > 0 ? (100.0 * counts[i] / total) : 0.0;
      info("  pool[{}]: {} jobs ({:.1f}%), {} pending", i, counts[i], pct,
           pending[i]);
    }
    // Reset counters after printing
    for (int i = 0; i < kBucketCount; ++i) {
      job_counts_[i].store(0, std::memory_order_relaxed);
    }
  }

private:
  std::array<std::unique_ptr<WorkerPool>, kBucketCount> pools_;
  std::array<std::atomic<size_t>, kBucketCount> job_counts_;
  std::array<std::atomic<size_t>, kBucketCount> pending_job_counts_;
};

} // namespace hogd

