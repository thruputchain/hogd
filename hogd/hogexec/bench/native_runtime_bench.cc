#include <benchmark/benchmark.h>

#include <optional>
#include <string>
#include <string_view>

#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

namespace {

constexpr std::string_view kMethodName = "0123456789ABCDEF";

std::optional<Runtime::MethodID>
parse_method_id_with_temp_string(std::string_view method_name)
{
  return Runtime::method_id_from_name(std::string(method_name));
}

void
bm_method_id_parse_current_path(benchmark::State &state)
{
  for (auto _ : state) {
    auto out = parse_method_id_with_temp_string(kMethodName);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(bm_method_id_parse_current_path)
    ->Unit(benchmark::kNanosecond); // NOLINT

void
bm_method_id_parse_direct(benchmark::State &state)
{
  for (auto _ : state) {
    auto out = Runtime::method_id_from_name(kMethodName);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(bm_method_id_parse_direct)->Unit(benchmark::kNanosecond); // NOLINT

} // namespace

} // namespace hogd

BENCHMARK_MAIN();
