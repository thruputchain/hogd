2026-03-13

- Optimized coroutine frame allocation in `x23/x23/corotrace.h` by replacing the aligned global `operator new`/`operator delete` path with a small manual cache-line-aligned allocator. `x23::PromiseBase` still returns frames aligned to `hardware_destructive_interference_size`, but avoids the heavier libc aligned-allocation path on every coroutine creation and destruction.
- Benchmark result on `/tmp/codex/hogd-head-baseline-build/x23/tests/bench` versus `/tmp/codex/hogd-build/x23/tests/bench` with `--benchmark_filter='synchronous_empty_coro|coawait_empty_async|time_async_return|time_maybesync_return_async' --benchmark_min_time=1s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`:
  `synchronous_empty_coro` improved from about `13.6 ns` to `9.03 ns`, `coawait_empty_async` from `13.9 ns` to `8.94 ns`, `time_async_return` from `18.9 ns` to `13.3 ns`, and `time_maybesync_return_async` from `19.5 ns` to `13.5 ns` CPU time.
- Validation:
  `scripts/build.sh -d /tmp/codex/hogd-build`
  `ctest --test-dir /tmp/codex/hogd-build/x23/tests --output-on-failure`

- Optimized the byte-backed `HexArray` constructors in `hogd/container/merkle/hex_array.h` to skip zero-initializing the entire backing store before copying input bytes. Those constructors always produce an even-length logical path, so the untouched suffix stays unobservable until later writes, and the old memset was pure overhead on every Merkle path materialization.
- Added a focused `bench_hex_array_from_bytes` benchmark in `hogd/container/merkle/merkle_trie_bench.cc` to keep the constructor cost measurable.
- Benchmark result on `/tmp/codex/hogd-build/hogd/container/merkle/merkle_trie_bench --benchmark_filter='bench_hex_array_from_bytes' --benchmark_min_time=1s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`:
  `bench_hex_array_from_bytes` improved from about `2.69 ns` to `0.80 ns` CPU time.
- Validation:
  `scripts/build.sh -d /tmp/codex/hogd-build`
  `/tmp/codex/hogd-build/hogd/container/merkle/hex_array_test`
  `/tmp/codex/hogd-build/hogd/container/merkle/node_test`
  `/tmp/codex/hogd-build/hogd/container/merkle/merkle_trie_test`

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
