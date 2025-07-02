#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"

namespace hogd {

namespace {

constexpr size_t kMin = 8;
constexpr size_t kMax = 8 << 10;

template<typename Func>
void
bench_hash(benchmark::State &state, Func hash_fn)
{
  if (!crypto_init())
    std::terminate();
  std::vector<std::byte> in(state.range(0));
  for (auto _ : state)
    hash_fn(in);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(state.range(0)));
}

void
bm_sha256(benchmark::State &state)
{
  bench_hash(state, [](std::span<std::byte> buf) { return sha256(buf); });
}
BENCHMARK(bm_sha256)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_libcrypto_sha256(benchmark::State &state)
{
  bench_hash(state, [](std::span<std::byte> buf) { return LibCrypto::sha256(buf); });
}
BENCHMARK(bm_libcrypto_sha256)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_libcrypto_sha512_256(benchmark::State &state)
{
  bench_hash(state, [](std::span<std::byte> buf) { return LibCrypto::sha512_256(buf); });
}
BENCHMARK(bm_libcrypto_sha512_256)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_crc32c(benchmark::State &state)
{
  bench_hash(state, [](std::span<std::byte> buf) { return crc32c(buf); });
}
BENCHMARK(bm_crc32c)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_siphash24(benchmark::State &state)
{
  bench_hash(state, [](std::span<std::byte> buf) {
    return SipHash24::get_default()(buf);
  });
}
BENCHMARK(bm_siphash24)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

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
