#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogexec/components/hash_buffers.h"

namespace hogd {
namespace {

constexpr std::array<std::byte, 32> kPayload = [] {
  std::array<std::byte, 32> out{};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(i);
  }
  return out;
}();

void
bm_hash_buffers_small_updates(benchmark::State &state)
{
  auto const chunk_size = static_cast<size_t>(state.range(0));
  auto const update_count = static_cast<size_t>(state.range(1));
  auto const chunk = std::span(kPayload).first(chunk_size);

  for (auto _ : state) {
    HashBuffers buffers;
    benchmark::DoNotOptimize(
        buffers.update(0, std::span<const std::byte>(chunk), true));
    for (size_t i = 1; i < update_count; ++i) {
      benchmark::DoNotOptimize(
          buffers.update(0, std::span<const std::byte>(chunk), false));
    }
    auto result = buffers.finalize(0);
    benchmark::DoNotOptimize(result);
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * update_count));
  state.SetBytesProcessed(
      static_cast<int64_t>(state.iterations() * update_count * chunk_size));
}

BENCHMARK(bm_hash_buffers_small_updates)
    ->Args({1, 64})
    ->Args({8, 64})
    ->Args({32, 64})
    ->Unit(benchmark::kNanosecond);

} // namespace
} // namespace hogd
