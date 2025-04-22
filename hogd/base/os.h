// os.h - support for OS-level features and resource monitoring

#pragma once

#include <bit>

#include "hogd/base/compiler.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"

namespace hogd {

namespace os_detail {

size_t available_parallelism();

} // namespace os_detail

// available_parallelism returns the maximum number of cores that this process
// is permitted to run on.
inline size_t
available_parallelism()
{
  alignas(kCacheLineSize) static size_t cores =
      os_detail::available_parallelism();
  return cores;
}

inline constexpr int kDefaultShardBaseOrder = 9;

// scalable_shard_order returns the log2 number of shards needed for good
// multicore scalability.
inline int
scalable_shard_order(int base = kDefaultShardBaseOrder)
{
  size_t cores = available_parallelism();
  return base + std::countr_zero(std::bit_ceil(cores));
}

// scalable_shard_size returns the number of shards needed for good multicore
// scalability.
inline size_t
scalable_shard_size(int base = kDefaultShardBaseOrder)
{
  return 1UL << scalable_shard_order(base);
}

struct ProcessStats {
  size_t utime;    // user mode jiffies
  size_t stime;    // kernel mode jiffies
  size_t iotime;   // block I/O jiffies
  size_t vsize;    // virtual memory size
  size_t rss;      // resident set memory size
  size_t nthreads; // number of threads
};

// read_proc_stat returns Linux-specific statistics about resource usage.
ErrnoStatus<ProcessStats> read_process_stats();

} // namespace hogd
