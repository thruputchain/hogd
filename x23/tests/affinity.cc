
#include <print>

#include "x23/threadpool.h"

int
main()
{
  auto l3map = x23::cpu_cache_map(3);
  for (const auto &[cpu,cache] : l3map)
    std::println("CPU {} -> Cache {}", cpu, cache);
}
