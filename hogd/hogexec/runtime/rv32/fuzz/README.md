# RV32 fuzz targets (local-only)

Two coverage-guided libFuzzer targets, built only with `-DBUILD_FUZZERS=ON`
(default OFF, excluded from `all`, no CI). Require a clang/libc++ toolchain.

- `rv32_fuzz_artifact` -- the artifact deploy path. Raw bytes exercise every
  header/digest rejection; a fixup phase assembles a well-formed artifact around
  fuzzer input to reach decode, run-gas lowering, fusion, and image staging.
- `rv32_fuzz_engine_diff` -- canonical `Machine` vs production
  `ThreadedMachine`. Asserts exact-`ProgramError` decoder parity, then execution
  parity (OOG class + `gas_remaining==0` first; full trace parity only when
  neither engine OOGs).

## Build

    source ./scripts/macos-env.sh
    ./scripts/build.sh -d build/clang -t Debug -c clang -n
    cmake -S . -B build/clang -DBUILD_FUZZERS=ON
    make -C build/clang rv32_fuzz_artifact rv32_fuzz_engine_diff

## Run

    ./build/clang/hogd/hogexec/runtime/rv32/rv32_fuzz_engine_diff \
      -max_total_time=60 hogd/hogexec/runtime/rv32/fuzz/corpus/engine_diff
    ./build/clang/hogd/hogexec/runtime/rv32/rv32_fuzz_artifact \
      -max_total_time=60 hogd/hogexec/runtime/rv32/fuzz/corpus/artifact

Divergences abort with a stderr diagnostic and reproducing words; libFuzzer
writes the input to `crash-*`. Reproduce with:

    ./build/clang/hogd/hogexec/runtime/rv32/rv32_fuzz_engine_diff crash-<hash>

## Regenerating seeds

    make -C build/clang rv32_emit_seeds
    ./build/clang/hogd/hogexec/runtime/rv32/rv32_emit_seeds \
      hogd/hogexec/runtime/rv32/fuzz/corpus

## Helper unit tests

    make -C build/clang rv32_fuzz_common_test
    ./build/clang/hogd/hogexec/runtime/rv32/rv32_fuzz_common_test
