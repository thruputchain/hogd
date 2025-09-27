#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
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
bench_func(benchmark::State &state, Func fn)
{
  std::vector<std::byte> in(state.range(0));
  for (auto _ : state)
    fn(in);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(state.range(0)));
}

void
bm_sha256(benchmark::State &state)
{
  bench_func(state, [](std::span<std::byte> buf) { return sha256(buf); });
}
BENCHMARK(bm_sha256)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_blake3(benchmark::State &state)
{
  bench_func(state, [](std::span<std::byte> buf) { return blake3(buf); });
}
BENCHMARK(bm_blake3)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_crc32c(benchmark::State &state)
{
  bench_func(state, [](std::span<std::byte> buf) { return crc32c(buf); });
}
BENCHMARK(bm_crc32c)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_siphash24(benchmark::State &state)
{
  bench_func(state, [](std::span<std::byte> buf) {
    return SipHash24::get_default()(buf);
  });
}
BENCHMARK(bm_siphash24)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_ed25519_sign(benchmark::State &state)
{
  auto [pk, sk] = ed25519_keygen();
  bench_func(state,
             [sk](std::span<std::byte> buf) { return ed25519_sign(sk, buf); });
}
BENCHMARK(bm_ed25519_sign)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_ed25519_sign_rust(benchmark::State &state)
{
  auto [pk, sk] = ed25519_keygen();
  bench_func(state,
             [sk](std::span<std::byte> buf) { return ed25519_sign_rust(sk, buf); });
}
BENCHMARK(bm_ed25519_sign_rust)->RangeMultiplier(2)->Range(kMin, kMax); // NOLINT

void
bm_ed25519_sign_and_verify(benchmark::State &state)
{
  auto [pk, sk] = ed25519_keygen();
  bench_func(state, [pk, sk](std::span<std::byte> buf) {
    auto sig = ed25519_sign(sk, buf);
    return ed25519_verify(pk, sig, buf);
  });
}
BENCHMARK(bm_ed25519_sign_and_verify)
    ->RangeMultiplier(2)
    ->Range(kMin, kMax); // NOLINT

void
bm_ed25519_sign_and_verify_rust(benchmark::State &state)
{
  auto [pk, sk] = ed25519_keygen();
  bench_func(state, [pk, sk](std::span<std::byte> buf) {
    auto sig = ed25519_sign_rust(sk, buf);
    return ed25519_verify_rust(pk, sig, buf);
  });
}
BENCHMARK(bm_ed25519_sign_and_verify_rust)
    ->RangeMultiplier(2)
    ->Range(kMin, kMax); // NOLINT
    

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
