#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <span>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/crypto/mldsa.h"
#include "hogd/crypto/mldsa_signer.h"
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
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_sha256)->RangeMultiplier(2)->Range(kMin, kMax);

void
bm_blake3(benchmark::State &state)
{
  bench_func(state, [](std::span<std::byte> buf) { return blake3(buf); });
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_blake3)->RangeMultiplier(2)->Range(kMin, kMax);

void
bm_crc32c(benchmark::State &state)
{
  bench_func(state, [](std::span<std::byte> buf) { return crc32c(buf); });
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_crc32c)->RangeMultiplier(2)->Range(kMin, kMax);

void
bm_siphash24(benchmark::State &state)
{
  bench_func(state, [](std::span<std::byte> buf) {
    return SipHash24::get_default()(buf);
  });
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_siphash24)->RangeMultiplier(2)->Range(kMin, kMax);

void
bm_ed25519_sign(benchmark::State &state)
{
  auto [pk, sk] = ed25519_keygen();
  bench_func(state, [sk](std::span<std::byte> buf) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    return ed25519_sign(sk, buf).value();
    // NOLINTEND(bugprone-unchecked-optional-access)
  });
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_ed25519_sign)->RangeMultiplier(2)->Range(kMin, kMax);

void
bm_ed25519_sign_and_verify(benchmark::State &state)
{
  auto [pk, sk] = ed25519_keygen();
  bench_func(state, [pk, sk](std::span<std::byte> buf) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto sig = ed25519_sign(sk, buf).value();
    // NOLINTEND(bugprone-unchecked-optional-access)
    return ed25519_verify(pk, sig, buf);
  });
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_ed25519_sign_and_verify)->RangeMultiplier(2)->Range(kMin, kMax);

// Verification alone, shaped like bm_mldsa65_verify so the two compare
// directly: what the VerifySignature syscall prices per scheme.
void
bm_ed25519_verify(benchmark::State &state)
{
  auto [pk, sk] = ed25519_keygen();
  std::vector<std::byte> msg(state.range(0));
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  auto sig = ed25519_sign(sk, msg).value();
  // NOLINTEND(bugprone-unchecked-optional-access)
  if (!ed25519_verify(pk, sig, msg)) {
    state.SkipWithError("signature does not verify");
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(ed25519_verify(pk, sig, msg));
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(state.range(0)));
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_ed25519_verify)->RangeMultiplier(2)->Range(kMin, kMax);

void
bm_mldsa65_verify(benchmark::State &state)
{
  auto key = MlDsa65SigningKey::generate();
  std::vector<std::byte> msg(state.range(0));
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  auto sig = key.sign(msg).value();
  // NOLINTEND(bugprone-unchecked-optional-access)
  if (!mldsa65_verify(key.public_key(), sig, msg)) {
    state.SkipWithError("signature does not verify");
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(mldsa65_verify(key.public_key(), sig, msg));
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(state.range(0)));
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_mldsa65_verify)->RangeMultiplier(2)->Range(kMin, kMax);

void
bm_lthash16_add(benchmark::State &state)
{
  bench_func(state, [](std::span<std::byte> buf) {
    LtHash16 h;
    h.add(buf);
    return h;
  });
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_lthash16_add)->RangeMultiplier(2)->Range(kMin, kMax);

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
