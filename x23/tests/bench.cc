
#include <x23/async.h>
#include <x23/event.h>
#include <x23/serialize.h>

#include <benchmark/benchmark.h>

#include "tests/xdrtest.hh"

using fix_4_100 = std::array<fix_4, 100>;

template<x23::is_xdr_type X>
void
serialize_xdr(benchmark::State &state)
{
  X x{};
  for (auto _ : state)
    x23::serialize(x);
}
BENCHMARK(serialize_xdr<std::tuple<>>);
BENCHMARK(serialize_xdr<fix_4>);
BENCHMARK(serialize_xdr<fix_4_100>);
BENCHMARK(serialize_xdr<u_4_12>);

template<x23::is_xdr_type X>
void
deserialize_xdr(benchmark::State &state)
{
  X x{};
  x23::Buf b = x23::serialize(x);
  for (auto _ : state)
    x23::deserialize(&x, b);
}
BENCHMARK(deserialize_xdr<std::tuple<>>);
BENCHMARK(deserialize_xdr<fix_4>);
BENCHMARK(deserialize_xdr<fix_4_100>);
BENCHMARK(deserialize_xdr<u_4_12>);

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
