#include <benchmark/benchmark.h>

#include <barrier>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/softstate/hashset_deduplicate.h"
#include "types.hh"

namespace hogd {

template<typename Deduplicator>
void
bm_deduplicate(benchmark::State &state)
{
  static Deduplicator *deduplicator;

  static std::barrier<> *sync_point;

  if (state.thread_index() == 0) {
    sync_point = new std::barrier(state.threads());
  }

  for (auto _ : state) {
    if (state.thread_index() == 0) {
      deduplicator = new Deduplicator();
    }

    sync_point->arrive_and_wait();
    state.ResumeTiming();

    uint64_t work_amount = state.range(0) / state.threads();
    for (uint64_t i = 0; i < work_amount; i++) {
      SHA256Hash hash;
      uint64_t val = i + (state.thread_index() * work_amount);
      std::memcpy(hash.data(), &val, sizeof(uint64_t));

      if (!deduplicator->try_insert(hash)) {
        std::terminate();
      }
    }

    sync_point->arrive_and_wait();
    state.PauseTiming();

    if (state.thread_index() == 0) {
      delete deduplicator;
    }
  }

  if (state.thread_index() == 0) {
    delete sync_point;
  }
}

BENCHMARK(bm_deduplicate<AtomicHashsetDeduplicator>)
    ->RangeMultiplier(4)
    ->Range(32, 32 << 8)
    ->ThreadRange(1, 32); // NOLINT
BENCHMARK(bm_deduplicate<HashsetDeduplicatorSimple>)
    ->RangeMultiplier(4)
    ->Range(32, 32 << 8)
    ->ThreadRange(1, 32); // NOLINT
BENCHMARK(bm_deduplicate<HashsetDeduplicatorSimpleV2>)
    ->RangeMultiplier(4)
    ->Range(32, 32 << 8)
    ->ThreadRange(1, 32); // NOLINT

} // namespace hogd

int
main(int argc, char *argv[])
{
  using hogd::GStatus;
  GStatus<void> ret = hogd::crypto_init();
  if (!ret)
    return EXIT_FAILURE;
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
