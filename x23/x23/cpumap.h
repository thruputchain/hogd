// -*- C++ -*-

#pragma once

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

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

// If field is "MyField", then returns "Value" from the first line in
// file of the form "MyField: Value".
std::string get_field(std::string_view field, std::filesystem::path file);

#if HAVE_SCHED_SETAFFINITY

extern const class CpuSet *allowed_cpus;

class CpuSet {
public:
  static constexpr unsigned kNCPUs =
      X23_CPUSET_NCPUS ? X23_CPUSET_NCPUS : CPU_SETSIZE;
  static constexpr auto kSize = CPU_ALLOC_SIZE(kNCPUs);
  struct uninitialized {};

  constexpr CpuSet(uninitialized) noexcept {}
  constexpr CpuSet() noexcept : CpuSet(uninitialized{}) { zero(); }

  cpu_set_t *set() noexcept { return reinterpret_cast<cpu_set_t *>(buf_); }
  const cpu_set_t *set() const noexcept
  {
    return reinterpret_cast<const cpu_set_t *>(buf_);
  }

  static constexpr size_t size() noexcept { return kSize; }
  static constexpr unsigned num_cpus() noexcept { return kNCPUs; }

  void zero() noexcept { CPU_ZERO_S(kSize, set()); }
  void set(unsigned cpu) noexcept
  {
    assert(cpu < kNCPUs);
    CPU_SET_S(cpu, kSize, set());
  }
  void clr(unsigned cpu) noexcept
  {
    assert(cpu < kNCPUs);
    CPU_CLR_S(cpu, kSize, set());
  }
  bool isset(unsigned cpu) const noexcept
  {
    assert(cpu < kNCPUs);
    return CPU_ISSET_S(cpu, kSize, set());
  }
  int count() const noexcept { return CPU_COUNT_S(kSize, set()); }

#define MAKE_BITOP(op, text)                                                   \
  CpuSet &operator op##=(const CpuSet & o) noexcept                            \
  {                                                                            \
    CPU_##text##_S(kSize, set(), set(), o.set());                              \
    return *this;                                                              \
  }                                                                            \
  friend CpuSet operator op(const CpuSet &a, const CpuSet &b) noexcept         \
  {                                                                            \
    CpuSet ret{uninitialized{}};                                               \
    CPU_##text##_S(kSize, ret.set(), a.set(), b.set());                        \
    return ret;                                                                \
  }
  MAKE_BITOP(&, AND)
  MAKE_BITOP(|, OR)
  MAKE_BITOP(^, XOR)
#undef MAKE_BITOP

  friend bool operator==(const CpuSet &a, const CpuSet &b) noexcept
  {
    return CPU_EQUAL_S(kSize, a.set(), b.set());
  }

  // True if the set contains at least one CPU we are allowed to run
  // on in our cgroup.
  bool valid(const CpuSet *allowed = allowed_cpus) const noexcept
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
} // namespace x23

template<> struct std::formatter<x23::CpuSet> {
  static constexpr auto parse(std::format_parse_context &ctx)
  {
    return ctx.begin();
  }

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
  bool valid() const noexcept { return true; }
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
#else  // !HAVE_SCHED_SETAFFINITY
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

// Construct one instance of type T for each CpuSet in a CpuMap.  If
// ValidOnly is false, then it will only construct a T for CpuSets on
// which we can actually be scheduled.
template<typename T, bool ValidOnly = false> class PerCpuSet {
public:
  using value_type = T;

  const CpuMap &map_;

  PerCpuSet(const CpuMap &map) : map_(map), values_(a.allocate(size()))
  {
    if constexpr (ValidOnly)
      constructed_.resize(map_.nbuckets());
  }
  ~PerCpuSet()
  {
    for (auto i : valid_indices_)
      std::destroy_at(values_ + i);
    a.deallocate(values_, map_.nbuckets());
  }

  size_t size() const { return map_.nbuckets(); }

  // Supply either constructor arguments or invocable arguments that
  // take a const CpuSet & as the final argument.  E.g.,
  //
  //   // default construct all elements
  //   percpu.populate();
  //
  //   // create each per-CpuSet object with a lambda
  //   percpu.populate([](const CpuSet &cs) { return create(cs); });
  template<typename... Args> void populate(Args &&...args)
  {
    // If this assert fails, you probably tried to populate twice
    assert(!valid_indices_.size());
    for (int i = 0; i < map_.nbuckets(); ++i) {
      const auto &s = map_.cpuset(i);
      if (!ValidOnly || s.valid())
        construct(i, std::forward<Args>(args)...);
    }
  }

  value_type &lookup() { return lookup(map_.bucket()); }
  value_type &lookup(int bucket)
  {
    assert(bucket < map_.nbuckets());
    if constexpr (ValidOnly)
      if (!constructed_[bucket]) {
        assert(valid_indices_.size());
        return values_[valid_indices_[bucket % valid_indices_.size()]];
      }
    return values_[bucket];
  }

  // Minimal support to allow range-for
  struct iterator {
    PerCpuSet *cs_;
    size_t idx_;

    T &operator*() const { return cs_->values_[cs_->valid_indices_[idx_]]; }
    iterator &operator++()
    {
      ++idx_;
      return *this;
    }
    friend bool operator==(const iterator &a, const iterator &b) = default;
  };
  iterator begin() { return iterator{.cs_ = this, .idx_ = 0}; }
  iterator end()
  {
    return iterator{.cs_ = this, .idx_ = valid_indices_.size()};
  }

private:
  static inline std::allocator<T> a;

  // Record which indices of values_ have been constructed
  [[no_unique_address]] std::conditional_t<ValidOnly, std::vector<bool>,
                                           std::tuple<>> constructed_;

  value_type *values_;          // Per-CpuSet values
  Vec<unsigned> valid_indices_; // List of valid indices to values_

  // Simple emplace
  template<typename... Args>
  void construct(size_t i, Args &&...args)
      requires std::constructible_from<value_type, Args...>
  {
    std::construct_at(values_ + i, std::forward<Args>(args)...);
    if constexpr (ValidOnly) {
      assert(!constructed_[i]);
      constructed_[i] = true;
    }
    valid_indices_.push_back(i);
  }

  // Emplace from invocation result on CpuSet
  template<typename F, typename... Args>
  requires std::invocable<F, Args..., const CpuSet &> &&
           std::constructible_from<
               value_type, std::invoke_result_t<F, Args..., const CpuSet &>>
  void construct(size_t i, F &&f, Args &&...args)
  {
    assert(!constructed_[i]);
    std::construct_at(values_ + i,
                      std::invoke(std::forward<F>(f),
                                  std::forward<Args>(args)..., map_.cpuset(i)));
    constructed_[i] = true;
    valid_indices_.push_back(i);
  }
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

template<typename T>
struct alignas(hardware_destructive_interference_size) CacheAligned : T {
  using T::T;
};

} // namespace x23
