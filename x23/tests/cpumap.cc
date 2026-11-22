#include <gtest/gtest.h>

#include "x23/cpumap.h"
#include "x23/vec.h"

namespace x23 {

#if HAVE_SCHED_SETAFFINITY

// Replace the process-wide allowed_cpus for one test, so populate()
// sees a given mask without changing this process's affinity.
struct ScopedAllowedCpus {
  CpuSet saved_;

  explicit ScopedAllowedCpus(const CpuSet &cs) : saved_(*allowed_cpus)
  {
    allowed_cpus.destroy();
    allowed_cpus.construct(cs);
  }
  ~ScopedAllowedCpus()
  {
    allowed_cpus.destroy();
    allowed_cpus.construct(saved_);
  }
};

// Every CPU that bucket() sends to a bucket must be in that bucket's
// CpuSet, including CPUs past the end of the map.
TEST(CpuMap, CpusetMatchesBucket)
{
  for (const auto &map : {CpuMap{Vec{0}}, CpuMap{Vec{0, 1}}}) {
    for (unsigned cpu : {0u, 1u, 2u, 3u, 7u, CpuSet::kNCPUs - 1}) {
      int b = map.bucket(cpu);
      EXPECT_TRUE(map.cpuset(b).isset(cpu)) << "cpu " << cpu << " bucket " << b;
    }
  }
}

// A single-bucket map serves a process that may not run on CPU 0.
TEST(PerCpuSet, SingleBucketWithoutCpu0)
{
  CpuSet cs;
  cs.set(1);
  ScopedAllowedCpus allowed(cs);

  CpuMap map{Vec{0}};
  EXPECT_TRUE(map.cpuset(0).valid());
  PerCpuSet<int, true> pcs(map, populate, 7);
  EXPECT_EQ(pcs.lookup(0), 7);
  EXPECT_EQ(pcs.lookup(), 7);
}

// With no bucket valid, lookup() must still find a value.
TEST(PerCpuSet, NoValidBucket)
{
  ScopedAllowedCpus allowed(CpuSet{});

  CpuMap map{Vec{0, 1}};
  PerCpuSet<int, true> pcs(map, populate, 7);
  EXPECT_EQ(pcs.lookup(0), 7);
  EXPECT_EQ(pcs.lookup(1), 7);
  int n = 0;
  for (int v : pcs) {
    EXPECT_EQ(v, 7);
    ++n;
  }
  EXPECT_EQ(n, 1);
}

#endif // HAVE_SCHED_SETAFFINITY

} // namespace x23
