#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <ranges>
#include <span>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/concurrent_map.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"

namespace hogd {

namespace {

struct HashWrapper {
  constexpr uint64_t operator()(uint64_t val) const
  {
    static SipHash24 hasher;
    return hasher(std::as_bytes(std::span(&val, 1)));
  }
};

void
bench_concurrent_map_insert_lowcontention(benchmark::State &state)
{
  std::vector<std::byte> in(state.range(0));

  WorkerPool &pool = WorkerPool::get_default_pool();

  size_t nworkers = state.range(0);
  uint64_t nvals = state.range(1);

  pool.adjust_workers(nworkers);

  for (auto _ : state) {
    ConcurrentMap<uint64_t, uint64_t, HashWrapper> map;
    pool.for_each_parallel(
        std::ranges::iota_view{0UL, nvals},
        [&map](size_t i) {
          uint64_t key = i;
          uint64_t val = i;
          CHECK(map.insert_or_get(key, val) != nullptr);
        },
        4000);
    map.clear();
  }
}

void
bench_synchronized_map_insert_lowcontention(benchmark::State &state)
{
  std::vector<std::byte> in(state.range(0));

  WorkerPool &pool = WorkerPool::get_default_pool();

  size_t nworkers = state.range(0);
  uint64_t nvals = state.range(1);

  pool.adjust_workers(nworkers);

  for (auto _ : state) {
    SynchronizedMap<uint64_t, uint64_t, HashWrapper> map;
    pool.for_each_parallel(
        std::ranges::iota_view{0UL, nvals},
        [&map](size_t i) {
          uint64_t key = i;
          uint64_t val = i;
          map.insert(key, val);
        },
        4000);
  }
}

BENCHMARK(bench_concurrent_map_insert_lowcontention)
    ->Args({1, 1'000'000})
    ->Args({4, 1000000})
    ->Args({8, 1'000'000})
    ->Args({16, 1'000'000})
    ->Args({32, 1'000'000})
    ->Args({64, 1'000'000})
    ->Args({128, 1'000'000})
    ->Args({256, 1'000'000})
    ->Args({1, 10'000'000})
    ->Args({4, 10'000'000})
    ->Args({8, 10'000'000})
    ->Args({16, 10'000'000})
    ->Args({32, 10'000'000})
    ->Args({64, 10'000'000})
    ->Args({128, 10'000'000})
    ->Args({256, 10'000'000})
    ->Args({128, 100'000'000})
    ->Args({256, 100'000'000})
    ->UseRealTime();

BENCHMARK(bench_synchronized_map_insert_lowcontention)
    ->Args({1, 1'000'000})
    ->Args({4, 1'000'000})
    ->Args({8, 1'000'000})
    ->Args({16, 1'000'000})
    ->Args({32, 1'000'000})
    ->Args({64, 1'000'000})
    ->Args({128, 1'000'000})
    ->Args({256, 1'000'000})
    ->Args({1, 10'000'000})
    ->Args({4, 10'000'000})
    ->Args({8, 10'000'000})
    ->Args({16, 10'000'000})
    ->Args({32, 10'000'000})
    ->Args({64, 10'000'000})
    ->Args({128, 10'000'000})
    ->Args({256, 10'000'000})
    ->Args({128, 100'000'000})
    ->Args({256, 100'000'000})
    ->UseRealTime();

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0)
    return EXIT_FAILURE;
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
