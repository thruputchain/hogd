# RV32 VM Benchmark Baseline

Running Release-build benchmark notes for the RV32 interpreter, runtime artifact
fixtures, wasmi comparison rows, and syscall bridge microbenchmarks.

Raw benchmark JSON is intentionally not committed. Keep the command, commit, host
metadata, and summarized rows here so later runs can be compared without treating
machine-local JSON as source.

## Regenerate

```sh
build-release-ccache-tmp/hogd/hogexec/runtime/rv32/rv32_vm_bench \
  --benchmark_min_time=0.5s \
  --benchmark_out=/tmp/rv32-vm.json \
  --benchmark_out_format=json
```

Use the same host and Release configuration for before/after claims. Google
Benchmark reports CPU scaling enabled on this Linux machine, so treat sub-3%
movement as likely noise unless the benchmark is specifically targeted.

## 2026-05-29 Linux Register-Native Bridge

- Commit: `fc0d9481fee6747b60848277a96d0632a7c66689`
- Build: `build-release-ccache-tmp`, Release
- Host: `hog12`, Linux x86_64, 128 CPUs, Google Benchmark v1.9.0
- Command output: `/tmp/rv32-native-bridge.json`
- Bridge ABI: register-native syscall number/args in `a0`-`a7`, no frame/footer

| Benchmark | Real time | CPU time | Counter |
| --- | ---: | ---: | --- |
| `bm_dispatch_arithmetic` | 0.00553 ms | 0.00544 ms | 74.1M RV32 instr/sec |
| `bm_load_store_loop` | 0.00623 ms | 0.00613 ms | 65.7M RV32 instr/sec |
| `bm_rv32_fibonacci_recursive_20` | 3.484 ms | 3.477 ms | 75.6M RV32 instr/sec |
| `bm_wasmi_wat_fibonacci_recursive_20` | 0.540 ms | 0.539 ms | 345.3M wasmi fuel/sec |
| `bm_wasmi_built_contract_fibonacci_recursive_20` | 0.655 ms | 0.654 ms | 311.7M wasmi fuel/sec |
| `bm_syscall_bridge_roundtrip/4096` | 0.000128 ms | 0.000128 ms | 7.81M calls/sec, 29.8 GiB/sec copied |
| `bm_syscall_bridge_roundtrip/65536` | 0.00206 ms | 0.00205 ms | 487.0k calls/sec, 29.7 GiB/sec copied |
| `bm_rv32_artifact_arithmetic_control_flow` | 0.00108 ms | 0.00108 ms | gas 30, guest gas 26, 1 ecall |
| `bm_rv32_artifact_memory_transform` | 0.00229 ms | 0.00229 ms | gas 123, guest gas 119, 1 ecall |
| `bm_rv32_artifact_compiled_sha256_return` | 0.00757 ms | 0.00757 ms | gas 25, guest gas 17, 2 ecalls |

For this host, RV32 Fibonacci is about `6.45x` slower than the repo-built wasmi
Fibonacci row by wall time. Do not compare RV32 instruction counters directly to
wasmi fuel counters; use wall time for cross-engine comparisons.

## Native Bridge Comparison

The pre-native comparison used the stale benchmark executable already present in
`build-release-ccache-tmp` before rebuilding. It still reported
`rv32.bridge_frame_bytes=104`, confirming it was the old memory-frame/footer
bridge. The native run rebuilt the same target at the commit above.

| Benchmark | Frame/footer real | Register-native real | Change | Notes |
| --- | ---: | ---: | ---: | --- |
| `bm_syscall_bridge_roundtrip/4096` | 157.4 ns | 128.3 ns | 18.5% faster | Clear targeted bridge win. |
| `bm_syscall_bridge_roundtrip/65536` | 2097.4 ns | 2057.9 ns | 1.9% faster | Mostly memory-copy bound. |
| `bm_rv32_artifact_compiled_sha256_return` | 7896.7 ns | 7574.8 ns | 4.1% faster | End-to-end host-call artifact improved; guest gas dropped `35 -> 17`. |

Read: the register-native bridge helps, but the remaining bridge cost is mostly
the scratch memory copy used to preserve host-call atomicity. That is why the
4 KiB bridge microbenchmark moves much more than the 64 KiB case.

## 2026-05-27 Initial Baseline

- Commit: `d61b564ed7090195cdc8594c62a407868139d92e`
- Build: `build-release`, `CMAKE_BUILD_TYPE=Release`

| Benchmark | Real time | CPU time | Counter |
| --- | ---: | ---: | --- |
| `bm_dispatch_arithmetic` | 0.00175 ms | 0.00175 ms | 230.8M RV32 instr/sec |
| `bm_load_store_loop` | 0.00174 ms | 0.00174 ms | 231.4M RV32 instr/sec |
| `bm_rv32_fibonacci_recursive_20` | 1.043 ms | 1.043 ms | 251.9M RV32 instr/sec |
| `bm_wasmi_wat_fibonacci_recursive_20` | 0.284 ms | 0.284 ms | 654.5M wasmi fuel/sec |
| `bm_wasmi_built_contract_fibonacci_recursive_20` | 0.360 ms | 0.360 ms | 566.8M wasmi fuel/sec |

For the repo-built `fibonacci_raw.wasm` artifact, RV32 was about `2.90x` slower
by wall time on that compute-heavy recursive Fibonacci microbenchmark.
