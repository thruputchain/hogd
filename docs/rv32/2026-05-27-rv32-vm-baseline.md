# RV32 VM Benchmark Baseline

Date: 2026-05-27
Commit: `d61b564ed7090195cdc8594c62a407868139d92e`
Build: `build-release`, `CMAKE_BUILD_TYPE=Release`

This records the first RV32 VM versus wasmi head-to-head baseline for the
experimental in-tree VM. The raw Google Benchmark JSON is not committed; it can
be regenerated with the command below.

## Command

```sh
build-release/hogd/hogexec/runtime/rv32/rv32_vm_bench \
  --benchmark_min_time=0.5s \
  --benchmark_out=docs/rv32/2026-05-27-rv32-vm-baseline.json \
  --benchmark_out_format=json
```

## Results

| Benchmark | Real time | CPU time | Throughput counter |
| --- | ---: | ---: | ---: |
| `bm_dispatch_arithmetic` | 0.00175 ms | 0.00175 ms | 230.8M RV32 instructions/sec |
| `bm_load_store_loop` | 0.00174 ms | 0.00174 ms | 231.4M RV32 instructions/sec |
| `bm_rv32_fibonacci_recursive_20` | 1.043 ms | 1.043 ms | 251.9M RV32 instructions/sec |
| `bm_wasmi_wat_fibonacci_recursive_20` | 0.284 ms | 0.284 ms | 654.5M wasmi fuel/sec |
| `bm_wasmi_built_contract_fibonacci_recursive_20` | 0.360 ms | 0.360 ms | 566.8M wasmi fuel/sec |

## Interpretation

For the repo-built `fibonacci_raw.wasm` artifact, the current RV32 interpreter is
about 2.90x slower by wall time on this compute-heavy recursive Fibonacci
microbenchmark.

The dispatch and load/store loop microbenches show similar RV32 throughput
around 230M executed guest instructions/sec, close to the 251.9M/sec Fibonacci
row, so the current baseline does not isolate the Fibonacci gap to
call/stack-frame handling alone.

The WAT benchmark is a minimal wasm text fixture compiled to `.wasm` during the
CMake build. It is useful as a pure engine-shape datapoint, but the
`built_contract` result is the better baseline for comparisons against existing
repo wasm benchmarks.

Throughput counters are not directly comparable across engines: RV32 reports
executed RV32 guest instructions, while wasmi reports consumed fuel. Use wall
time for the head-to-head viability comparison.

## Environment Notes

Google Benchmark could not read a stable CPU frequency on this macOS host and
reported a thread-affinity warning. Those warnings affect metadata quality, not
the timing columns, but future comparisons should prefer the same machine and
Release configuration.
