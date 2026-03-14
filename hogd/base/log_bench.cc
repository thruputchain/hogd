#include <benchmark/benchmark.h>

#include <string_view>

#include "hogd/base/log.h"

namespace hogd {

namespace {

void
bm_source_basename(benchmark::State &state)
{
  constexpr std::string_view kPath =
      "/very/long/source/tree/hogd/base/some/deeply/nested/log.cc";
  for (auto _ : state) {
    auto filename = detail::source_basename(kPath);
    benchmark::DoNotOptimize(filename);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(bm_source_basename); // NOLINT

} // namespace

} // namespace hogd

BENCHMARK_MAIN();
