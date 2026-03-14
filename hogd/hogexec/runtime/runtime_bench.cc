#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <span>
#include <string_view>

#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/init.h"

namespace hogd {

namespace {

class NoOpRuntime : public Runtime {
public:
  NoOpRuntime() : Runtime(make_new_host_call_context(nullptr)) {}

  [[nodiscard]] uint64_t get_available_gas() const noexcept override
  {
    return available_gas_;
  }

  void set_available_gas(uint64_t gas) noexcept override
  {
    available_gas_ = gas;
  }

  std::span<std::byte> get_memory() noexcept override { std::terminate(); }

private:
  InvokeStatus<void> invoke_internal(std::string_view method_name) override
  {
    benchmark::DoNotOptimize(method_name);
    return {};
  }

  uint64_t available_gas_ = 0;
};

void
bm_runtime_invoke_method_id(benchmark::State &state)
{
  NoOpRuntime runtime;
  constexpr Runtime::MethodID kMethod = 0x1234'5678'90AB'CDEF;

  for (auto _ : state) {
    auto result = runtime.invoke(kMethod, std::numeric_limits<uint64_t>::max());
    benchmark::DoNotOptimize(result);
  }
}

void
bm_runtime_invoke_method_id_legacy(benchmark::State &state)
{
  NoOpRuntime runtime;
  constexpr Runtime::MethodID kMethod = 0x1234'5678'90AB'CDEF;

  for (auto _ : state) {
    auto result = runtime.invoke(std::format("{:016X}", kMethod),
                                 std::numeric_limits<uint64_t>::max());
    benchmark::DoNotOptimize(result);
  }
}

void
bm_runtime_invoke_string_view(benchmark::State &state)
{
  NoOpRuntime runtime;
  constexpr std::array<char, 16> kMethodName = {'1', '2', '3', '4', '5', '6',
                                                '7', '8', '9', '0', 'A', 'B',
                                                'C', 'D', 'E', 'F'};

  for (auto _ : state) {
    auto result =
        runtime.invoke(std::string_view(kMethodName.data(), kMethodName.size()),
                       std::numeric_limits<uint64_t>::max());
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK(bm_runtime_invoke_method_id_legacy)->Unit(benchmark::kNanosecond);
BENCHMARK(bm_runtime_invoke_method_id)->Unit(benchmark::kNanosecond);
BENCHMARK(bm_runtime_invoke_string_view)->Unit(benchmark::kNanosecond);

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0) {
    return EXIT_FAILURE;
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
