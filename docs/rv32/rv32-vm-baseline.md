# RV32 VM Benchmark Baseline

Initial Release-build RV32 interpreter versus wasmi baseline.

- Date: 2026-05-27
- Commit: `d61b564ed7090195cdc8594c62a407868139d92e`
- Build: `build-release`, `CMAKE_BUILD_TYPE=Release`

## Regenerate

```sh
build-release/hogd/hogexec/runtime/rv32/rv32_vm_bench \
  --benchmark_min_time=0.5s \
  --benchmark_out=docs/rv32/rv32-vm-baseline.json \
  --benchmark_out_format=json
```

The raw JSON is not committed.

## Results

| Benchmark | Real time | CPU time | Counter |
| --- | ---: | ---: | ---: |
| `bm_dispatch_arithmetic` | 0.00175 ms | 0.00175 ms | 230.8M RV32 instructions/sec |
| `bm_load_store_loop` | 0.00174 ms | 0.00174 ms | 231.4M RV32 instructions/sec |
| `bm_rv32_fibonacci_recursive_20` | 1.043 ms | 1.043 ms | 251.9M RV32 instructions/sec |
| `bm_wasmi_wat_fibonacci_recursive_20` | 0.284 ms | 0.284 ms | 654.5M wasmi fuel/sec |
| `bm_wasmi_built_contract_fibonacci_recursive_20` | 0.360 ms | 0.360 ms | 566.8M wasmi fuel/sec |

## Read

For the repo-built `fibonacci_raw.wasm` artifact, RV32 was about `2.90x` slower
by wall time on this compute-heavy recursive Fibonacci microbenchmark.

Do not compare the counters directly across engines. RV32 reports guest
instructions/sec; wasmi reports fuel/sec. Use wall time for the head-to-head
baseline.

Future comparisons should use the same host and Release configuration. Google
Benchmark reported CPU-frequency and thread-affinity warnings on the original
macOS run; those affect metadata quality more than the timing columns.
