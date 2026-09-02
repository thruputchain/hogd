#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <x23/async.h>
#include <x23/event.h>
#include <x23/serialize.h>
#include <x23/serialize_cxs.h>
#include <x23/vec.h>

#include <benchmark/benchmark.h>

#include "tests/xdrtest.hh"

using fix_4_100 = std::array<fix_4, 100>;

template<x23::is_xdr_type X>
void
serialize_xdr(benchmark::State &state)
{
  X x{};
  for (auto _ : state) {
    benchmark::DoNotOptimize(x);
    x23::Buf b = x23::serialize(x);
    benchmark::DoNotOptimize(b.data());
    benchmark::ClobberMemory();
  }
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
  for (auto _ : state) {
    benchmark::DoNotOptimize(b);
    x23::deserialize(&x, b);
    benchmark::DoNotOptimize(x);
  }
}
BENCHMARK(deserialize_xdr<std::tuple<>>);
BENCHMARK(deserialize_xdr<fix_4>);
BENCHMARK(deserialize_xdr<fix_4_100>);
BENCHMARK(deserialize_xdr<u_4_12>);

void
serialize_cxs_integers(benchmark::State &state)
{
  std::array<uint64_t, 100> values;
  for (size_t i = 0; i < values.size(); ++i)
    values[i] = (uint64_t{1} << (i % 64)) - 1;
  for (auto _ : state) {
    benchmark::DoNotOptimize(values);
    auto encoded = x23::serialize_cxs(values);
    benchmark::DoNotOptimize(encoded);
    if (encoded) {
      benchmark::DoNotOptimize(encoded->data());
      benchmark::DoNotOptimize(encoded->size());
      benchmark::ClobberMemory();
    }
  }
}
BENCHMARK(serialize_cxs_integers);

void
deserialize_cxs_integers(benchmark::State &state)
{
  std::array<uint64_t, 100> values;
  for (size_t i = 0; i < values.size(); ++i)
    values[i] = (uint64_t{1} << (i % 64)) - 1;
  auto serialized = x23::serialize_cxs(values);
  if (!serialized) {
    state.SkipWithError("CXS serialization failed");
    return;
  }
  x23::Buf encoded = std::move(*serialized);
  benchmark::DoNotOptimize(encoded.data());
  benchmark::ClobberMemory();
  for (auto _ : state) {
    benchmark::DoNotOptimize(encoded);
    auto status = x23::deserialize_cxs(&values, encoded);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(values);
  }
}
BENCHMARK(deserialize_cxs_integers);

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
  [&] -> x23::Async<void> {
    for (auto _ : state)
      co_await empty();
    co_return;
  }()();
}
BENCHMARK(coawait_empty_async);

x23::ThreadPool pool;

x23::Async<uint64_t>
migrate_loaded_task(size_t migrations)
{
  uint64_t thread_handoffs = 0;
  auto previous = std::this_thread::get_id();
  for (size_t i = 0; i < migrations; ++i) {
    co_await pool.bg();
    auto current = std::this_thread::get_id();
    thread_handoffs += current != previous;
    previous = current;
  }
  co_return thread_handoffs;
}

x23::Async<uint64_t>
migrate_loaded_round(size_t coroutines, size_t migrations)
{
  std::vector<x23::Async<uint64_t>> tasks;
  tasks.reserve(coroutines);
  for (size_t i = 0; i < coroutines; ++i)
    tasks.emplace_back(migrate_loaded_task(migrations));

  uint64_t thread_handoffs = 0;
  for (auto &task : tasks)
    thread_handoffs += co_await task;
  co_return thread_handoffs;
}

void
migrate_coro(benchmark::State &state)
{
  [&] -> x23::Async<void> {
    for (auto _ : state)
      co_await pool.bg();
    co_return;
  }()();
}
BENCHMARK(migrate_coro)->UseRealTime()->MeasureProcessCPUTime();

void
migrate_coro_count(benchmark::State &state)
{
  uint64_t thread_handoffs = 0;

  [&] -> x23::Async<void> {
    auto previous = std::this_thread::get_id();

    for (auto _ : state) {
      co_await pool.bg();

      auto current = std::this_thread::get_id();
      thread_handoffs += current != previous;
      previous = current;
    }
    co_return;
  }()();

  state.counters["handoffs/iter"] =
      benchmark::Counter{double(thread_handoffs),
                         benchmark::Counter::kAvgIterations};
}

void
migrate_coro_threads(benchmark::State &state)
{
  x23::ThreadPool local_pool(state.range(0));

  uint64_t thread_handoffs = 0;
  [&] -> x23::Async<void> {
    auto previous = std::this_thread::get_id();
    for (auto _ : state) {
      co_await local_pool.bg();
      auto current = std::this_thread::get_id();
      thread_handoffs += current != previous;
      previous = current;
    }
    co_return;
  }()();

  state.counters["handoffs/iter"] =
      benchmark::Counter{double(thread_handoffs),
                         benchmark::Counter::kAvgIterations};
}

void
migrate_coro_pingpong(benchmark::State &state)
{
  x23::ThreadPool pools[]{x23::ThreadPool(1), x23::ThreadPool(1)};
  uint64_t thread_handoffs = 0;

  [&] -> x23::Async<void> {
    auto previous = std::this_thread::get_id();
    unsigned target = 0;
    for (auto _ : state) {
      co_await pools[target].bg();
      target ^= 1;
      auto current = std::this_thread::get_id();
      thread_handoffs += current != previous;
      previous = current;
    }
    co_return;
  }()();

  state.counters["handoffs/iter"] =
      benchmark::Counter{double(thread_handoffs),
                         benchmark::Counter::kAvgIterations};
}

void
migrate_coro_loaded(benchmark::State &state)
{
  // The 4x and 16x cases keep runqueues populated, measuring scheduler
  // throughput instead of idle-worker wakeup latency.
  constexpr uint64_t kMigrationsPerIteration = 1'000'000;
  const auto coroutines = uint64_t(state.range(0));
  const auto migrations_per_coro =
      (kMigrationsPerIteration + coroutines - 1) / coroutines;
  const auto migrations_per_iteration = coroutines * migrations_per_coro;
  uint64_t thread_handoffs = 0;

  for (auto _ : state)
    thread_handoffs += migrate_loaded_round(coroutines, migrations_per_coro)();

  const auto total_migrations = migrations_per_iteration * state.iterations();
  state.SetItemsProcessed(total_migrations);
  state.counters["handoffs/migration"] =
      double(thread_handoffs) / double(total_migrations);
}

[[maybe_unused]]
static const auto _migrate_coro_registration = [] {
  auto *loaded =
      benchmark::RegisterBenchmark("migrate_coro_loaded", migrate_coro_loaded)
          ->UseRealTime()
          ->MeasureProcessCPUTime();
  if (getenv("X23_BENCH_MIGRATE")) {
    BENCHMARK(migrate_coro_count)
        ->UseRealTime()
        ->MeasureProcessCPUTime()
        ->Iterations(1'000'000)
        ->Repetitions(10);
    BENCHMARK(migrate_coro_threads)
        ->Arg(1)
        ->Arg(2)
        ->Arg(8)
        ->Arg(x23::nproc())
        ->UseRealTime()
        ->MeasureProcessCPUTime()
        ->Iterations(100'000)
        ->Repetitions(5);
    BENCHMARK(migrate_coro_pingpong)
        ->UseRealTime()
        ->MeasureProcessCPUTime()
        ->Iterations(100'000)
        ->Repetitions(5);
    loaded->Arg(x23::nproc())
        ->Arg(4 * x23::nproc())
        ->Arg(16 * x23::nproc())
        ->Iterations(5)
        ->Repetitions(5);
  }
  else
    loaded->Arg(4 * x23::nproc());
  return false;
}();

void
time_async_return(benchmark::State &state)
{
  x23::Callback<int> dummy{[](int) {}};
  for (auto _ : state)
    x23::async_return(0)(dummy);
}
BENCHMARK(time_async_return);

void
time_sync_return(benchmark::State &state)
{
  x23::Callback<int> dummy{[](int) {}};
  for (auto _ : state)
    [](auto cb) { cb(0); }(dummy);
}
BENCHMARK(time_sync_return);

void
time_maybesync_return_sync(benchmark::State &state)
{
  x23::Callback<int> dummy{[](int) {}};
  for (auto _ : state)
    // Specifying <int> works around gcc internal compiler error
    x23::MaybeAsync<int>{0}(dummy);
}
BENCHMARK(time_maybesync_return_sync);

void
time_maybesync_return_async(benchmark::State &state)
{
  x23::Callback<int> dummy{[](int) {}};
  for (auto _ : state)
    // Specifying <int> works around gcc internal compiler error
    x23::MaybeAsync<int>{x23::async_return(0)}(dummy);
}
BENCHMARK(time_maybesync_return_async);

template<typename T>
void
time_append_range(benchmark::State &state)
{
  const std::vector<std::uint8_t> bytes(51, 0xc5);
  T target;
  for (auto _ : state)
    if constexpr (requires { target.append_range(bytes); })
      target.append_range(bytes);
    else
      target.insert(target.end(), bytes.begin(), bytes.end());
}
BENCHMARK(time_append_range<std::vector<std::uint8_t>>);
BENCHMARK(time_append_range<x23::Buf>);

BENCHMARK_MAIN();
