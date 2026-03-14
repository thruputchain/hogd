2026-03-13

- Reduced `hogd::Scheduler::schedule()` overhead in [hogd/mempool/scheduler.cc] by removing a redundant `std::map::lower_bound()` from the per-account inner loop. The scheduler only needs to drop expired transactions once before it starts walking an account's runqueue; previously it restarted the tree search from the beginning after every scheduled transaction.
- Added a focused `bench_scheduler_schedule_single_account` benchmark in `hogd/mempool/test/bench.cc` to measure the case where one account has a deep runqueue, which exposed the repeated-search hotspot that the existing one-transaction-per-account schedule benchmark did not.
- Benchmark result on `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_schedule_single_account/256'`:
  the 256-transaction single-account schedule case improved from about `0.57 ms` to `0.45 ms` CPU time across `perf stat -r 5` runs, while the existing aggregate benchmark stayed in the same range (`bench_scheduler_schedule/256`: `0.53 ms` CPU time on the post-change run).
- Validation:
  `scripts/build.sh -d /tmp/codex2/build`
  `/tmp/codex2/build/hogd/mempool/test/mempool_test`
  `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_schedule_single_account|bench_scheduler_schedule/256' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`
  `perf stat -r 5 /tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_schedule_single_account/256'`

- Reduced `hogd::Scheduler::add_txs_with_prefetched_accounts()` overhead in [hogd/mempool/scheduler.cc] by lowering the background coroutine cap from 128 to 32 after profiling still showed the 256-transaction enqueue benchmark spending most of its CPU time in thread-pool futex waits and wakeups. The previous cap-to-work-size change removed empty workers, but 128 wakeups were still too expensive for medium and large batches.
- Added a focused `bench_scheduler_schedule` benchmark in `hogd/mempool/test/bench.cc` so the scheduler path can be measured separately from enqueue and transfer processing when checking for regressions.
- Benchmark result on `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_add_txs|bench_scheduler_process_transfers|bench_scheduler_schedule' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`:
  `bench_scheduler_add_txs/64` improved from about `0.76 ms` to `0.37 ms` CPU time, and `bench_scheduler_add_txs/256` from about `1.42 ms` to `0.47 ms`, while the schedule and transfer benchmarks stayed in the same range (`bench_scheduler_schedule/256`: `0.59 ms` to `0.60 ms`, `bench_scheduler_process_transfers/256`: `0.87 ms` to `0.82 ms`).
- Validation:
  `scripts/build.sh -d /tmp/codex2/build`
  `/tmp/codex2/build/hogd/mempool/test/mempool_test`
  `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_add_txs|bench_scheduler_process_transfers|bench_scheduler_schedule' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

- Reduced `hogd::Scheduler::process_transfers()` fan-out in [hogd/mempool/scheduler.cc] from 100 workers to 32 after profiling showed the 256-delta benchmark still spending most of its CPU time in thread-pool wakeups and futex contention rather than transfer math. The old split gave many workers only a couple of deltas each, so scheduler overhead dominated.
- Benchmark result on `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_process_transfers' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`:
  `bench_scheduler_process_transfers/64` improved from about `1.06 ms` to `0.78 ms` CPU time, and `bench_scheduler_process_transfers/256` from about `1.43 ms` to `0.83 ms`, while the smaller cases stayed roughly flat (`1`: `0.47 ms` to `0.44 ms`, `8`: `0.57 ms` to `0.57 ms`).
- Validation:
  `scripts/build.sh -d /tmp/codex2/build`
  `/tmp/codex2/build/hogd/mempool/test/mempool_test`
  `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_process_transfers' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

- Reduced `hogd::Scheduler::process_transfers()` overhead in [hogd/mempool/scheduler.cc] by capping the number of background coroutines to the number of transfer deltas instead of always launching 100 workers. Small transfer batches were paying nearly constant thread-pool wakeup overhead because most workers received empty slices.
- Added a focused transfer-processing benchmark in `hogd/mempool/test/bench.cc` that preloads cached account ages and measures only the scheduler transfer path, so the fixed worker-launch cost is visible without the RAM DB harness.
- Benchmark result on `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_process_transfers' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`:
  `bench_scheduler_process_transfers/1` improved from about `1.43 ms` to `0.49 ms` CPU time, `bench_scheduler_process_transfers/8` from about `1.38 ms` to `0.58 ms`, and `bench_scheduler_process_transfers/64` from about `1.37 ms` to `1.07 ms`, while the large-batch case stayed roughly flat (`256` deltas: `1.42 ms` to `1.46 ms`).
- Validation:
  `scripts/build.sh -d /tmp/codex2/build`
  `/tmp/codex2/build/hogd/mempool/test/mempool_test`
  `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_process_transfers' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

- Reduced `hogd::Scheduler::add_txs()` enqueue overhead in [hogd/mempool/scheduler.cc] by capping the number of background coroutines to the amount of real work instead of always launching 128 workers. Small and medium transaction batches were paying nearly constant scheduler overhead because most of those coroutines immediately found empty slices.
- Added a focused enqueue benchmark in `hogd/mempool/test/bench.cc` that preloads account ages and measures only the post-lookup insertion path, so the scheduler hot path is observable without the RAM DB test harness dominating results.
- Benchmark result on `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_add_txs' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`:
  `bench_scheduler_add_txs/1` improved from about `1.08 ms` to `0.016 ms` CPU time, `bench_scheduler_add_txs/8` from about `1.09 ms` to `0.097 ms`, and `bench_scheduler_add_txs/64` from about `1.14 ms` to `0.70 ms`, while the large-batch case stayed effectively flat (`256` txs: `1.23 ms` to `1.27 ms`).
- Validation:
  `scripts/build.sh -d /tmp/codex2/build`
  `/tmp/codex2/build/hogd/mempool/test/mempool_test`
  `/tmp/codex2/build/hogd/mempool/test/mempool_bench --benchmark_filter='bench_scheduler_add_txs' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

- Added a ready-result fast path to `x23::Async<T>::operator()(Callback)` in `x23/x23/async.h`. Previously every callback invocation spawned a trampoline coroutine even when the `Async` had already completed, which added avoidable overhead to immediate-return paths such as `async_return()`.
- Benchmark result on `/tmp/hogd-codex-build/x23/tests/bench --benchmark_filter='time_async_return|time_sync_return|time_maybesync_return_async|time_maybesync_return_sync' --benchmark_min_time=1s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`:
  `time_async_return` improved from about `32.0 ns` to `19.2 ns`, and `time_maybesync_return_async` improved from about `30.8 ns` to `18.6 ns`, while the synchronous baselines stayed flat (`time_sync_return` at `4.00 ns`, `time_maybesync_return_sync` at `1.33 ns`).
- Validation:
  `scripts/build.sh -d /tmp/hogd-codex-build`
  `ctest --output-on-failure -R '^(callback|sync|detached|shutdown|rpc|rpc_dispatch_race)$'` in `/tmp/hogd-codex-build/x23/tests` (`callback`, `sync`, and `detached` passed; `rpc`, `rpc_dispatch_race`, and `shutdown` were blocked by sandbox socket restrictions)
  `/tmp/hogd-codex-build/x23/tests/test_callback`
  `/tmp/hogd-codex-build/x23/tests/test_cleanup`

- Optimized `x23::detail::Serializer::putbytes()` in `x23/x23/serialize.h` to copy payload bytes first and then zero only the XDR padding bytes. The old path zeroed an overlapping 4-byte word before the copy for non-4-byte payload sizes, which added avoidable stores on every padded string or opaque byte vector.
- Added focused byte-serialization benchmarks in `x23/tests/bench.cc` to keep the work observable and compare aligned versus padded payload sizes.
- Benchmark result on `/tmp/hogd-codex-build/x23/tests/bench --benchmark_filter='serialize_bufview' --benchmark_min_time=1s`:
  `serialize_bufview<51>` improved from about `1.36 ns` to `0.86 ns` CPU time, while `serialize_bufview<48>` stayed effectively flat (`0.85 ns` to `0.84 ns`).
- Validation:
  `scripts/build.sh -d /tmp/hogd-codex-build`
  `/tmp/hogd-codex-build/x23/tests/test_xdr`
  `/tmp/hogd-codex-build/x23/tests/test_deserialize`
  `/tmp/hogd-codex-build/x23/tests/test_uiobuf`
