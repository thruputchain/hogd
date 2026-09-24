# RV32 fuzz targets

Two libFuzzer targets exercise artifact admission and engine agreement. They
require clang/libc++ and `BUILD_FUZZERS=ON`, and are excluded from the default
build. The [scheduled workflow](../../../../../.github/workflows/fuzz_rv32_engine_diff.yml)
runs both from the committed seed corpus.

- `rv32_fuzz_artifact` feeds raw bytes to the artifact parsers. It also repairs
  headers and digests around fuzzer-supplied sections to reach instruction
  decoding, lowering, fusion, and initial-memory preparation.
- `rv32_fuzz_engine_diff` compares the canonical and production engines. It
  checks exact decoder error classes and the production preparation verdict.
  Error locations can differ because canonical validation scans forwards and
  production lowering scans backwards. Without gas exhaustion, execution checks
  compare traces with intermediate gas removed, then compare final gas and the
  untraced production machine's final state. On exhaustion, all engines must
  report out-of-gas with zero remaining gas. Host call logs must match in both
  cases.

Inputs choose code, method ID, initial memory and its load address, and host
responses. Small run-gas caps exercise run splitting in short programs. The
scripted host derives results from its seed and call index, including return
values, gas debits, halts, out-of-gas, and guest-memory writes. It never returns
`HostStatus::kTrap`, which the threaded engine forbids.

## Build and run

Run from the repository root. The macOS environment helper applies to macOS;
use the repository's clang build environment on Linux.

```sh
source ./scripts/macos-env.sh
./scripts/build.sh -d build/clang -t Debug -c clang -n
cmake -S . -B build/clang -DBUILD_FUZZERS=ON
cmake --build build/clang --target rv32_fuzz_artifact rv32_fuzz_engine_diff

./build/clang/hogd/hog_exec/runtime/rv32/rv32_fuzz_engine_diff \
  -max_total_time=60 hogd/hog_exec/runtime/rv32/fuzz/corpus/engine_diff
./build/clang/hogd/hog_exec/runtime/rv32/rv32_fuzz_artifact \
  -max_total_time=60 hogd/hog_exec/runtime/rv32/fuzz/corpus/artifact
```

Divergences abort with a diagnostic and reproducing words. libFuzzer writes
`crash-*` inputs; pass one to the same binary to reproduce. Corpus directories
may gain new inputs during a run; use a copy for disposable local campaigns.

## Seed generation and helper tests

```sh
cmake --build build/clang --target rv32_emit_seeds rv32_fuzz_common_test
./build/clang/hogd/hog_exec/runtime/rv32/rv32_emit_seeds \
  hogd/hog_exec/runtime/rv32/fuzz/corpus
./build/clang/hogd/hog_exec/runtime/rv32/rv32_fuzz_common_test
```
