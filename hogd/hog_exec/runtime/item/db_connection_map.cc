#include "hogd/hog_exec/runtime/item/db_connection_map.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sched.h>
#endif

#include "hogd/base/os.h"
#include "x23/cpumap.h"
#include "x23/vec.h"

namespace hogd {

int
db_connection_count()
{
  return std::max(1, x23::core_map->nbuckets() / 2);
}

// Neither map x23 ships lands on the right count.  Measured on the bench
// machine under the current reply policy, against the connection count the
// executor had before:
//
//   connections   payments (median)   multiexec (median)
//   8  (llc_map)  1.85x slower        0.37-0.58x
//   24 (fixed)    baseline            baseline
//   32 (here)     0.98x               0.99x
//   64 (core_map) 1.02x               0.92x
//
// 8 does not merely run slower, it pins: throughput is flat within 4% across
// batch sizes 1, 5 and 10, where wider counts convert bigger batches into
// 1.4-1.6x more throughput.  That is a per-connection ceiling, and it is why
// the count cannot follow last-level cache -- llc_map is a single bucket on a
// machine whose cores share one L3.
//
// 64 clears that ceiling but gives some back: 0.92x on multiexec, and on
// payments it widens the tail on the largest configuration (max/median 1.38x
// against 1.09x here) while regressing the coroutine-driven mid-range.
//
// Half a core splits them.  It keeps the high-thread payments gain that 64
// showed (0.85-0.89x at 512 and 1024 threads) and stays level with the old
// fixed count on multiexec.
//
// The measurements above come from one machine profile.  What they establish
// is that the optimum is interior and that this count is right for a 64-core
// box; that it should scale with core count is the inference, not the
// measurement.  The count is logged at construction so any other profile
// reports what it resolved to.
//
// Which CPUs share a connection is a separate question from how many there
// are.  Dealing CPUs out round-robin (cpu i to connection i % n) put CPUs 0
// and 32 on one connection: two different L3s on an 8-CCD EPYC.  Each
// connection's xid lock was then taken from two LLCs, and since its reader
// resumes callers inline and DatabaseProxy's bg() requeues them by the
// reader's cpuid(), a reply could move a coroutine to the other LLC.  So
// connections are split between LLCs by CPU count, and the CPUs of one LLC
// take adjacent connections in core order, keeping SMT siblings on the same
// connection.
x23::Vec<int>
db_connection_buckets(const x23::Vec<int> &allowed, int nbuckets,
                      const x23::CpuMap &llc, const x23::CpuMap &core)
{
  // LLC groups in order of their first CPU.
  std::vector<std::vector<int>> groups;
  std::map<int, std::size_t> group_of;
  for (int cpu : allowed) {
    auto [it, fresh] = group_of.try_emplace(llc.bucket(cpu), groups.size());
    if (fresh)
      groups.emplace_back();
    groups[it->second].push_back(cpu);
  }
  for (auto &g : groups)
    std::ranges::stable_sort(g, {},
                             [&core](int cpu) { return core.bucket(cpu); });

  // Ids outside the mask go to bucket 0; every bucket gets at least one
  // allowed CPU, so each one is valid and gets a connection.
  x23::Vec<int> buckets;
  buckets.resize(std::ranges::max(allowed) + 1, 0);
  const int ngroups = static_cast<int>(groups.size());

  // Fewer connections than LLCs: some connection has to span LLCs, but no
  // LLC is split.
  if (nbuckets <= ngroups) {
    for (int i = 0; i < ngroups; i++)
      for (int cpu : groups[i])
        buckets[cpu] = i % nbuckets;
    return buckets;
  }

  // One connection per LLC, the rest in proportion to the LLC's CPU count.
  // A share never exceeds its group's size, so no bucket is left empty.
  const int total = static_cast<int>(allowed.size());
  std::vector<int> share(ngroups, 1);
  int given = 0;
  for (int i = 0; i < ngroups; i++) {
    share[i] +=
        (nbuckets - ngroups) * static_cast<int>(groups[i].size()) / total;
    given += share[i];
  }
  for (int i = 0; given < nbuckets; i = (i + 1) % ngroups)
    if (std::cmp_less(share[i], groups[i].size())) {
      share[i]++;
      given++;
    }

  int base = 0;
  for (int i = 0; i < ngroups; i++) {
    const auto &g = groups[i];
    const int n = static_cast<int>(g.size());
    for (int j = 0; j < n; j++)
      buckets[g[j]] = base + (j * share[i] / n);
    base += share[i];
  }
  return buckets;
}

const x23::CpuMap &
db_connection_map()
{
  static const x23::CpuMap map = [] {
    const int nconnections = db_connection_count();
    // The CPU ids this process may run on. They need not be 0..n-1: with an
    // affinity of 8-15, a map over ids 0-7 has no allowed CPU in any bucket,
    // the multiclient builds no connection, and the first lookup divides by
    // zero.
    const int ncpu = std::max(1, static_cast<int>(available_parallelism()));
    x23::Vec<int> allowed;
#ifdef __linux__
    cpu_set_t mask;
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0)
      for (int id = 0; id < CPU_SETSIZE; id++)
        if (CPU_ISSET(id, &mask))
          allowed.push_back(id);
#endif
    if (allowed.empty())
      for (int i = 0; i < ncpu; i++)
        allowed.push_back(i);
    // ncpu also honors DEFAULT_THREADPOOL_SIZE.
    const int nbuckets =
        std::min({nconnections, ncpu, static_cast<int>(allowed.size())});
    return x23::CpuMap{db_connection_buckets(allowed, nbuckets, *x23::llc_map,
                                             *x23::core_map)};
  }();
  return map;
}

} // namespace hogd
