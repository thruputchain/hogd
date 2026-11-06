#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
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

// One LLC's lane: a ThreadPool whose workers run only on that LLC's allowed
// CPUs, and a CoroPoll for the LLC's sockets, its poll thread pinned there
// too, that resumes coroutines in that pool.
//
// default_threadpool() keeps a run queue per LLC, but its workers are not
// pinned, a job moves to another LLC's queue when that queue is staler, and
// an idle worker steals from any queue, so a transaction's coroutines run on
// whichever L3 is free and its caches follow it around.  On the multiexec
// benchmark (64 cores, 8 L3s) one hog-exec ran about 30% faster with each
// connection's work in one lane, much as Whitney's one hog-exec per L3 did.
// Dealing requests rather than connections over the lanes kept only a third
// of that: a request then crosses L3s between its reader and its lane.
struct LlcLane {
  LlcLane(int llc, const x23::CpuSet &cpus, size_t ncpus, unsigned cpu)
    : llc(llc), cpu(cpu), pool(ncpus, cpus), cp(pool, cpus)
  {}

  const int llc;
  // A CPU in the lane, to queue work onto the lane's LLC.
  const unsigned cpu;
  x23::ThreadPool pool;
  PinnedCoroPoll cp;
};

// The lanes, one per LLC this process may run on.  There are none where
// there is one such LLC, since default_threadpool() is then already one, where
// DEFAULT_THREADPOOL_SIZE sets the worker count, or where CPU affinity is
// unavailable.  Built on first use and never destroyed: work may still be
// queued on them when the process exits.
class LlcLanes {
public:
  static LlcLanes &get()
  {
    static auto *lanes = new LlcLanes;
    return *lanes;
  }

  // llc's lane, or nullptr if it has none.
  LlcLane *at(int llc) const
  {
    if (llc < 0 || static_cast<size_t>(llc) >= by_llc_.size())
      return nullptr;
    return by_llc_[llc].get();
  }

  // The next lane round robin, or nullptr if there are none.
  LlcLane *next()
  {
    if (lanes_.empty())
      return nullptr;
    return lanes_[next_.fetch_add(1, std::memory_order_relaxed) %
                  lanes_.size()];
  }

private:
  LlcLanes()
  {
    x23::init_cpumaps();
#if HAVE_SCHED_SETAFFINITY
    std::vector<std::pair<int, x23::CpuSet>> llcs;
    size_t ncpus = 0;
    for (int i = 0; i < x23::llc_map->nbuckets(); i++)
      if (auto cpus = x23::llc_map->cpuset(i) & *x23::allowed_cpus;
          cpus.count()) {
        ncpus += static_cast<size_t>(cpus.count());
        llcs.emplace_back(i, cpus);
      }
    // The lanes run one worker per allowed CPU.  A DEFAULT_THREADPOOL_SIZE
    // that asks for another count (available_parallelism()), as the
    // single-thread stress tests do, keeps everything on default_threadpool(),
    // which honors it.
    if (llcs.size() < 2 || available_parallelism() != ncpus)
      return;
    by_llc_.resize(x23::llc_map->nbuckets());
    for (auto &[llc, cpus] : llcs) {
      unsigned cpu = 0;
      while (!cpus.isset(cpu))
        cpu++;
      by_llc_[llc] = std::make_unique<LlcLane>(llc, cpus, cpus.count(), cpu);
      lanes_.push_back(by_llc_[llc].get());
    }
    info("{} LLC lanes", lanes_.size());
#endif // HAVE_SCHED_SETAFFINITY
  }

  std::vector<std::unique_ptr<LlcLane>> by_llc_;
  std::vector<LlcLane *> lanes_;
  std::atomic<size_t> next_{0};
};

// The CoroPoll for sockets whose callers run on one LLC: its lane's, so a
// reply resumes its caller in the caller's lane.  default_coropoll() if the
// LLC has no lane.
inline x23::CoroPoll &
llc_coropoll(int llc)
{
  if (LlcLane *lane = LlcLanes::get().at(llc))
    return lane->cp;
  return default_coropoll();
}

// The pool the calling thread works for, so that a hop to the background
// stays in the caller's lane; default_threadpool() outside any pool.
inline x23::ThreadPool &
current_threadpool()
{
  if (x23::ThreadPool *pool = x23::ThreadPool::get_thread_pool())
    return *pool;
  return default_threadpool();
}

// The first CPU in cpus this process may run on, if any.
inline std::optional<unsigned>
first_allowed_cpu([[maybe_unused]] const x23::CpuSet &cpus)
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
