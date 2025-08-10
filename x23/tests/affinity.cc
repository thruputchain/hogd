/*
 * Note: an easy way to test on subsets of CPUs on linux is to run, e.g.:
 *
 *    systemd-run --scope -p AllowedCPUs=0-1,4-5 ./test_affinity
 */

#include <print>

#include "x23/threadpool.h"

int
main()
{
  // auto cpumap = x23::make_cpu_map("cache/index3/shared_cpu_map");
  // auto cpumap = x23::make_cpu_map("topology/core_id");
  std::println("nproc is {}", x23::nproc());
  auto &cpumap = *x23::llc_map;
  for (size_t i = 0; i < cpumap.get_map().size(); ++i)
    std::println("CPU {} -> Bucket {}", i, cpumap.get_map()[i]);
#if HAVE_SCHED_SETAFFINITY
  for (int i = 0; i < cpumap.nbuckets(); ++i) {
    auto allowed = cpumap.cpuset(i) & *x23::allowed_cpus;
    //auto allowed{*x23::allowed_cpus};
    //allowed &= cpumap.cpuset(i);
    if (allowed.count())
      std::println("Bucket {} -> CpuSet {} [{} allowed]", i, cpumap.cpuset(i),
		   allowed);
    else
      std::println("Bucket {} -> CpuSet {} [prohibited]", i, cpumap.cpuset(i));
  }
#endif // HAVE_SCHED_SETAFFINITY
  std::println("{} total buckets", cpumap.nbuckets());

  int cpuid = x23::cpuid();
  std::println("currently running on {} -> {}", cpuid, cpumap.bucket(cpuid));
}
