#pragma once

#include <array>
#include <cassert>
#include <future>
#include <type_traits>

#include "hogd/base/worker_pool.h"
#include "x23/cpumap.h"

namespace hogd {

// DispatchPool routes job submissions across multiple WorkerPools based on
// CPU LLC (Last-Level Cache) affinity. This reduces contention on the
// single mutex-protected queue in WorkerPool by distributing submissions
// across 8 separate pools.
//
// Each WorkerPool has nthreads/8 threads, where nthreads is the total
// number of worker threads requested.
class DispatchPool {
public:
  // Create a DispatchPool with nthreads total worker threads distributed
  // across 8 WorkerPools.
  explicit DispatchPool(int nthreads)
  {
    x23::init_cpumaps();
    int threads_per_pool = (nthreads + 7) / 8; // round up
    for (int i = 0; i < 8; ++i) {
      pools_[i] = std::make_unique<WorkerPool>(threads_per_pool);
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

  // submit enqueues a job to the WorkerPool corresponding to the current CPU's
  // LLC bucket and returns a future for when the job completes.
  template<typename Func,
           typename Ret = std::invoke_result_t<std::decay_t<Func>>>
  requires std::is_invocable_r_v<Ret, Func>
  std::future<Ret> submit(Func &&func)
  {
    // Route to pool based on current CPU's LLC bucket
    int bucket = x23::llc_map->bucket();
    int pool_idx = bucket % 8;
    return pools_[pool_idx]->submit(std::forward<Func>(func));
  }

private:
  std::array<std::unique_ptr<WorkerPool>, 8> pools_;
};

} // namespace hogd

