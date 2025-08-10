// -*- C++ -*-

#pragma once

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>

#include <x23/config.h>
#include <x23/err.h>
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

// If field is MyField, then returns "Value" from the first line
// of the form "MyField: Value" in file.
std::string get_field(std::string_view field, std::filesystem::path file);

#if HAVE_SCHED_SETAFFINITY

extern const class CpuSet *allowed_cpus;

class CpuSet {
public:
  static constexpr unsigned kNCPUs =
      X23_CPUSET_NCPUS ? X23_CPUSET_NCPUS : CPU_SETSIZE;
  static constexpr auto kSize = CPU_ALLOC_SIZE(kNCPUs);
  struct uninitialized {};

  constexpr CpuSet(uninitialized) {}
  constexpr CpuSet() : CpuSet(uninitialized{}) { zero(); }

  cpu_set_t *set() { return reinterpret_cast<cpu_set_t *>(buf_); }
  const cpu_set_t *set() const
  {
    return reinterpret_cast<const cpu_set_t *>(buf_);
  }

  static constexpr size_t size() { return kSize; }
  static constexpr unsigned num_cpus() { return kNCPUs; }

  void zero() { CPU_ZERO_S(kSize, set()); }
  void set(unsigned cpu)
  {
    assert(cpu < kNCPUs);
    CPU_SET_S(cpu, kSize, set());
  }
  void clr(unsigned cpu)
  {
    assert(cpu < kNCPUs);
    CPU_CLR_S(cpu, kSize, set());
  }
  bool isset(unsigned cpu) const
  {
    assert(cpu < kNCPUs);
    return CPU_ISSET_S(cpu, kSize, set());
  }
  int count() const { return CPU_COUNT_S(kSize, set()); }

#define MAKE_BITOP(op, text)                                                   \
  CpuSet &operator op##=(const CpuSet & o)                                     \
  {                                                                            \
    CPU_##text##_S(kSize, set(), set(), o.set());                              \
    return *this;                                                              \
  }                                                                            \
  friend CpuSet operator op(const CpuSet &a, const CpuSet &b)                  \
  {                                                                            \
    CpuSet ret{uninitialized{}};                                               \
    CPU_##text##_S(kSize, ret.set(), a.set(), b.set());                        \
    return ret;                                                                \
  }
  MAKE_BITOP(&, AND)
  MAKE_BITOP(|, OR)
  MAKE_BITOP(^, XOR)
#undef MAKE_BITOP

  friend bool operator==(const CpuSet &a, const CpuSet &b)
  {
    return CPU_EQUAL_S(kSize, a.set(), b.set());
  }

  // True if the set contains at least one CPU we are allowed to run
  // on in our cgroup.
  bool valid(const CpuSet *allowed = allowed_cpus) const
  {
    return allowed ? (*this & *allowed).count() : count();
  }

  // Set thread to run in a particular CpuSet.  If pid == 0 affects
  // the current thread.  Otherwise, pid must be the value returned by
  // gettid() in the thread, which you might not be able to obtain
  // from a std::thread.
  Errno set_affinity(pid_t pid = 0) const;

private:
  alignas(cpu_set_t) char buf_[kSize];
};
}

template<> struct std::formatter<x23::CpuSet> {
  constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }

  auto format(const x23::CpuSet &cs, auto &ctx) const
  {
    auto out = ctx.out();
    bool first = true;
    for (int i = 0, n = cs.num_cpus(); i < n;) {
      if (!cs.isset(i)) {
        ++i;
        continue;
      }
      int start = i;
      do {
        ++i;
      } while (i < n && cs.isset(i));
      if (first)
        first = false;
      else
        *out++ = ',';
      if (start == i - 1)
        out = std::format_to(out, "{}", start);
      else
        out = std::format_to(out, "{}-{}", start, i - 1);
    }
    return out;
  }
};

namespace x23 {

CpuSet parse_cpuset(std::string_view list);

inline CpuSet
get_allowed_cpus()
{
  return parse_cpuset(get_field("Cpus_allowed_list", "/proc/self/status"));
}

#else // !HAVE_SCHED_SETAFFINITY

// Dummy version if we don't have CPU sets
class CpuSet {
public:
  static constexpr unsigned kNCPUs = 1;
  bool valid() const { return true; }
  Errno set_affinity() const { return 0; }
};

#endif // !HAVE_SCHED_SETAFFINITY

class CpuMap {
public:
  explicit CpuMap(Vec<int> map) noexcept
    : map_(std::move(map)), nbuckets_(1 + std::ranges::max(map_))
  {
#if HAVE_SCHED_SETAFFINITY
    compute_cpusets();
#endif // HAVE_SCHED_SETAFFINITY
  }
  explicit constexpr CpuMap(std::nullptr_t) : nbuckets_(-1) {}

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

  const CpuSet &cpuset(int i) const
  {
    assert(i >= 0 && i < nbuckets_);
#if HAVE_SCHED_SETAFFINITY
    return cpusets_[i];
#else // !HAVE_SCHED_SETAFFINITY
    static const CpuSet dummy;
    return dummy;
#endif // !HAVE_SCHED_SETAFFINITY
  }

private:
  Vec<int> map_;
  int nbuckets_;
#if HAVE_SCHED_SETAFFINITY
  Vec<CpuSet> cpusets_;

  void compute_cpusets();
#endif // HAVE_SCHED_SETAFFINITY
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

extern const CpuMap *core_map;
extern const CpuMap *llc_map;

// Call this function to ensure core_map and llc_map are initialized
// during static initialization.  Once main starts, they will be
// initialized anyway.
void init_cpumaps();

} // namespace x23
