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

// The single bucket holds every CPU we may run on, and nothing else.
TEST(CpuMap, SingleBucket)
{
  CpuSet cs;
  cs.set(1);
  cs.set(3);
  ScopedAllowedCpus allowed(cs);

  auto map = make_single_bucket_map();
  EXPECT_EQ(map.nbuckets(), 1);
  for (int cpu : {0, 1, 2, 3, 7})
    EXPECT_EQ(map.bucket(cpu), 0);
  EXPECT_TRUE(map.cpuset(0) == cs);
}

// A single-bucket map serves a process that may not run on CPU 0.
TEST(PerCpuSet, SingleBucketWithoutCpu0)
{
  CpuSet cs;
  cs.set(1);
  ScopedAllowedCpus allowed(cs);

  auto map = make_single_bucket_map();
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
