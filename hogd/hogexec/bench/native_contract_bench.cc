#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/hogexec/runtime/native_runtime.h"

namespace hogd {
namespace {

using MethodID = Runtime::MethodID;

NativeContract::function_t
make_noop_function()
{
  return [](NativeSyscallContext &) -> InvokeStatus<void> { return {}; };
}

std::vector<MethodID>
make_method_ids(size_t count)
{
  std::vector<MethodID> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.push_back(0x1000 + i * 0x10);
  }
  return out;
}

std::unordered_map<MethodID, NativeContract::function_t>
make_function_map(const std::vector<MethodID> &method_ids)
{
  std::unordered_map<MethodID, NativeContract::function_t> out;
  out.reserve(method_ids.size());
  for (auto method_id : method_ids) {
    out.emplace(method_id, make_noop_function());
  }
  return out;
}

void
bm_native_contract_lookup_legacy(benchmark::State &state)
{
  auto method_ids = make_method_ids(state.range(0));
  auto functions = make_function_map(method_ids);
  size_t index = 0;

  for (auto _ : state) {
    auto it = functions.find(method_ids[index]);
    benchmark::DoNotOptimize(it);
    benchmark::DoNotOptimize(it->second);
    index = (index + 1) % method_ids.size();
  }
}
BENCHMARK(bm_native_contract_lookup_legacy)
    ->Arg(1)
    ->Arg(4)
    ->Arg(10)
    ->Unit(benchmark::kNanosecond);

void
bm_native_contract_lookup(benchmark::State &state)
{
  auto method_ids = make_method_ids(state.range(0));
  NativeContract contract(make_function_map(method_ids));
  size_t index = 0;

  for (auto _ : state) {
    auto *function = contract.find_function(method_ids[index]);
    benchmark::DoNotOptimize(function);
    benchmark::DoNotOptimize(*function);
    index = (index + 1) % method_ids.size();
  }
}
BENCHMARK(bm_native_contract_lookup)
    ->Arg(1)
    ->Arg(4)
    ->Arg(10)
    ->Unit(benchmark::kNanosecond);

} // namespace
} // namespace hogd

BENCHMARK_MAIN();
