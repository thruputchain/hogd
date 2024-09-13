
#include <x23/async.h>
#include <x23/event.h>

#include <benchmark/benchmark.h>

void
empty_function(benchmark::State &state)
{
  void (*volatile empty)() = +[] {};
  for (auto _ : state)
    empty();
}
BENCHMARK(empty_function);

void
synchronous_empty_coro(benchmark::State &state)
{
  static constexpr auto empty = +[] -> x23::Async<void> { co_return; };
  for (auto _ : state)
    empty()();
}
BENCHMARK(synchronous_empty_coro);

void
coawait_empty_async(benchmark::State &state)
{
  static constexpr auto empty = +[] -> x23::Async<void> { co_return; };
  [&]->x23::Async<void> {
    for (auto _ : state)
      co_await empty();
    co_return;
  }()();
}
BENCHMARK(coawait_empty_async);

x23::ThreadPool pool;
void
migrate_coro(benchmark::State &state)
{
  [&] -> x23::Async<void> {
    for(auto _ : state)
      co_await pool.bg();
    co_return;
  }()();
}
BENCHMARK(migrate_coro);

BENCHMARK_MAIN();
