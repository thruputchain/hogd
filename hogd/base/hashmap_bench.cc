#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <span>
#include <stdexcept>

#include "hogd/base/hashmap.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"

namespace hogd {

namespace {

struct TestInt64Object {
  int64_t value;

  std::span<const std::byte> get_key() const
  {
    return std::as_bytes(std::span(&value, 1));
  }
};

void
bm_hashmap_insert(benchmark::State &state)
{
  for (auto _ : state) {
    HashMap<TestInt64Object> map(10);
    for (int64_t value = 0; value < state.range(0); value++) {
      map.emplace(std::as_bytes(std::span(&value, 1)), TestInt64Object{value});
    }
  }
}
BENCHMARK(bm_hashmap_insert)
    ->RangeMultiplier(2)
    ->Range(2 << 5, 2 << 20); // NOLINT

void
bm_hashmap_read(benchmark::State &state)
{
  HashMap<TestInt64Object> map(10);
  for (int64_t value = 0; value < state.range(0); value++) {
    map.emplace(std::as_bytes(std::span(&value, 1)), TestInt64Object{value});
  }
  for (auto _ : state) {
    for (int64_t value = 0; value < state.range(0); value++) {
      if (!map.modify_element(std::as_bytes(std::span(&value, 1)),
                              [](auto &v) { return v.value; })) {
        throw std::runtime_error("mismatch");
      }
    }
  }
}
BENCHMARK(bm_hashmap_read)
    ->RangeMultiplier(2)
    ->Range(2 << 5, 2 << 20); // NOLINT

} // namespace

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
