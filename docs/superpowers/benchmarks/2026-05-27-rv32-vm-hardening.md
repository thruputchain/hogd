# RV32 VM Hardening Pass

Date: 2026-05-27
Commit under test: `42c97b7fa241c434f7ffa72d16ffb37af8d91872`

This records the quick sanitizer confidence pass before starting the M2 host
bridge.

## Sanitizer Build

```sh
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_BENCHMARKS=OFF \
  -DWITH_JEMALLOC=OFF \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'

cmake --build build-asan --target rv32_vm_test -j8
ctest --test-dir build-asan -R '^rv32\.' --output-on-failure
```

## Result

`rv32_vm_test` built with ASAN/UBSAN instrumentation and all 28 RV32 tests
passed. No ASAN, UBSAN, or leak diagnostics were observed.

## Caveat

UBSAN used its default recover mode. A stricter future gate can add
`-fno-sanitize-recover=all` and `UBSAN_OPTIONS=print_stacktrace=1`, but this
pass is sufficient as a quick pre-M2 hardening check.
