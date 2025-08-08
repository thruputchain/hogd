// -*- C++ -*-

#pragma once

#include <x23/config.h>
#include <x23/utility.h>
#include <x23/vec.h>

#if HAVE_SCHED_GETCPU
#include <sched.h>
#endif // HAVE_SCHED_GETCPU

namespace x23 {

// Return the number of available hardware threads.  Tries to figure
// it out from Cpus_allowed in /proc/self/status, which works on
// modern linux distributions, but then falls back to
// std::thread::hardware_concurrency().
unsigned nproc();

inline unsigned
cpuid() noexcept
{
#if HAVE_SCHED_GETCPU
  return sched_getcpu();
#else  // !HAVE_SCHED_GETCPU
  static const std::hash<std::thread::id> hasher;
  return hasher(std::this_thread::get_id());
#endif // !HAVE_SCHED_GETCPU
}

class CpuMap {
public:
  CpuMap(Vec<int> map) noexcept
    : map_(std::move(map)), nbuckets_(1 + std::ranges::max(map_))
  {}
  constexpr CpuMap(std::nullptr_t) : map_(), nbuckets_(-1) {}

  int nbuckets() const { return nbuckets_; }
  const Vec<int> &get_map() const { return map_; }
  int bucket(int n = cpuid()) const noexcept
  {
#if HAVE_SCHED_GETCPU
    if (size_t(n) < map_.size()) [[likely]]
      return map_[n];
#endif // HAVE_SCHED_GETCPU
    return map_[size_t(n) % map_.size()];
  }

private:
  Vec<int> map_;
  int nbuckets_;
};

// Return a mapping of CPU numbers to buckets on linux.  For a given
// argument PATH, iterates over the following file for each CPU N:
//
//     /sys/devices/system/cpu/cpuN/PATH
//
// The file is expected to contain a unique string by which CPUs
// should be bucketed.  The following PATHs are useful:
//
//          core - "topology/core_id"
//     L1d cache - "cache/index0/shared_cpu_map"
//     L1i cache - "cache/index1/shared_cpu_map"
//      L2 cache - "cache/index2/shared_cpu_map"
//      L3 cache - "cache/index3/shared_cpu_map"
//
// Note that it doesn't actually parse the shared_cpu_map, files, just
// expects them to be unique per bucket.  Some linux machines have a
// "cache/index[0-3]/id", but this seems to be relatively recent, so
// it's safer to go by the shared_cpu_map file.
//
// Returns a dummy single-bucket mapping if the id files do not exist.
CpuMap make_cpu_map(std::string_view path);

// Calls make_cpu_map for the last level cache, which is generally
// cache/index3/shared_cpu_map or cache/index2/shared_cpu_map.
CpuMap make_llc_map();

extern CpuMap core_map;
extern CpuMap llc_map;

// Call this function to ensure core_map and llc_map are initialized
// during static initialization.  Once main starts, they will be
// initialized anyway.
void init_cpumaps();

} // namespace x23
