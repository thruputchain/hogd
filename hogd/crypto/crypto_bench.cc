#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"

namespace hogd {

void
bm_sha256(benchmark::State &state)
{
  if (!crypto_init())
    std::terminate();
  std::vector<std::byte> in(state.range(0));
  for (auto _ : state)
    sha256(in);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(state.range(0)));
}
BENCHMARK(bm_sha256)->RangeMultiplier(2)->Range(8, 8 << 10); // NOLINT

void
bm_siphash24(benchmark::State &state)
{
  if (!crypto_init())
    std::terminate();
  std::vector<std::byte> in(state.range(0));
  Siphash24Key key = siphash24_keygen();
  for (auto _ : state)
    siphash24(in, key);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(state.range(0)));
}
BENCHMARK(bm_siphash24)->RangeMultiplier(2)->Range(8, 8 << 10); // NOLINT

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
