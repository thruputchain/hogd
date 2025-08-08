
#include <print>

#include "x23/threadpool.h"

int
main()
{
  // auto cpumap = x23::make_cpu_map("cache/index3/shared_cpu_map");
  // auto cpumap = x23::make_cpu_map("topology/core_id");
  auto cpumap = x23::make_llc_map();
  for (size_t i = 0; i < cpumap.get_map().size(); ++i)
    std::println("CPU {} -> Bucket {}", i, cpumap.get_map()[i]);

  std::println("{} total buckets", cpumap.nbuckets());

  int cpuid = x23::cpuid();
  std::println("currently running on {} -> {}", cpuid, cpumap.bucket(cpuid));
}
