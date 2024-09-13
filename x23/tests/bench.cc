
#include <x23/async.h>
#include <x23/event.h>

#include <benchmark/benchmark.h>

void
BM_empty_function(benchmark::State &state)
{
  static constexpr auto empty = +[] {};
  for (auto _ : state)
    empty();
}
BENCHMARK(BM_empty_function);

void
BM_empty_coro(benchmark::State &state)
{
  static constexpr auto empty = +[] -> x23::Async<void> { co_return; };
  for (auto _ : state)
    empty()();
}
BENCHMARK(BM_empty_coro);

x23::ThreadPool pool;
void
BM_migrate(benchmark::State &state)
{
  auto mka = [&] -> x23::Async<void> {
    for(auto _ : state)
      co_await pool.bg();
    co_return;
  };
  auto a = mka();
  a();
}
BENCHMARK(BM_migrate);

BENCHMARK_MAIN();
