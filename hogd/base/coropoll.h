#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"

#include <x23/config.h>
#include <x23/cpumap.h>
#include <x23/debug.h>
#include <x23/event.h>
#include <x23/threadpool.h>

namespace hogd {

inline x23::ThreadPool &
default_threadpool()
{
  static x23::ThreadPool pool{available_parallelism()};
  return pool;
}

inline x23::CoroPoll &
default_coropoll()
{
  static x23::CoroPoll cp{default_threadpool()};
  return cp;
}

// A CoroPoll whose poll thread runs only on cpus.
class PinnedCoroPoll : public x23::CoroPoll {
public:
  PinnedCoroPoll(x23::ThreadPool &pool, const x23::CpuSet &cpus)
    : CoroPoll(pool, no_loop_thread{})
  {
    pollthread_ = std::thread([this, cpus] {
      cpus.set_affinity();
      loop();
    });
  }
};

// The CoroPoll for sockets whose callers run on one LLC: one per LLC, its
// poll thread pinned to that LLC's allowed CPUs.
//
// default_coropoll() is a single epoll thread for every socket in the
// process.  Every RPC reply that has to wait for its socket goes through it,
// so at hog-exec's reply rate it is a shared queueing point, and it runs
// wherever the scheduler puts it.  Per-LLC polls split that load and keep
// a connection's readiness handling on its callers' LLC.
inline x23::CoroPoll &
llc_coropoll(int llc)
{
  static const auto polls = [] {
    x23::init_cpumaps();
    std::vector<std::unique_ptr<x23::CoroPoll>> v(x23::llc_map->nbuckets());
#if HAVE_SCHED_SETAFFINITY
    for (int i = 0; i < x23::llc_map->nbuckets(); i++) {
      auto cpus = x23::llc_map->cpuset(i) & *x23::allowed_cpus;
      if (cpus.count())
        v[i] = std::make_unique<PinnedCoroPoll>(default_threadpool(), cpus);
    }
#endif // HAVE_SCHED_SETAFFINITY
    return v;
  }();
  if (llc < 0 || static_cast<size_t>(llc) >= polls.size() || !polls[llc])
    return default_coropoll();
  return *polls[llc];
}

// The first CPU in cpus this process may run on, if any.
inline std::optional<unsigned>
first_allowed_cpu(const x23::CpuSet &cpus)
{
#if HAVE_SCHED_SETAFFINITY
  for (unsigned cpu = 0; cpu < x23::CpuSet::num_cpus(); cpu++)
    if (cpus.isset(cpu) && x23::allowed_cpus->isset(cpu))
      return cpu;
#endif // HAVE_SCHED_SETAFFINITY
  return std::nullopt;
}

// Inclusive-exclusive half-open range used when slicing transaction batches.
struct ChunkRange {
  size_t start;
  size_t end; // [start, end)
};

inline std::vector<ChunkRange>
make_chunks(size_t total_size, size_t chunk_count)
{
  std::vector<ChunkRange> chunks;
  if (!total_size || !chunk_count) {
    return chunks;
  }
  chunk_count = std::min(chunk_count, total_size);

  const size_t base_size = total_size / chunk_count;
  const size_t remainder = total_size % chunk_count;

  size_t offset = 0;
  chunks.reserve(chunk_count);
  for (size_t i = 0; i < chunk_count; ++i) {
    // Distribute remainder across the first [remainder] chunks to keep them as
    // balanced as possible.
    const size_t chunk_size = base_size + (i < remainder ? 1 : 0);
    const size_t start = offset;
    const size_t end = start + chunk_size;
    chunks.push_back(ChunkRange{.start = start, .end = end});
    offset = end;
  }
  CHECK_EQ(offset, total_size);

  return chunks;
}

inline std::vector<ChunkRange>
make_chunks_for_max_size(size_t total_size, size_t max_chunk_size)
{
  if (!max_chunk_size) {
    return {};
  }
  // Round up so we never exceed [max_chunk_size] in any bucket.
  const size_t chunk_count = (total_size + max_chunk_size - 1) / max_chunk_size;
  return make_chunks(total_size, chunk_count);
}

} // namespace hogd
