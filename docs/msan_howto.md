# MemorySanitizer (MSan) — How-To

MSan detects reads of uninitialized memory. Unlike UBSan/ASan, MSan requires
**all** code to be instrumented — any uninstrumented library (OpenSSL, system
libc++) will produce false positives. This guide covers the full setup.

## Prerequisites (one-time, on the Linux build machine)

### 1. Build MSan-instrumented libc++

The system libc++ is not instrumented, so we build our own from LLVM source:

```bash
./scripts/build_msan_libcxx.sh $HOME/msan-libcxx
```

This takes ~10 minutes. It sparse-checkouts the matching LLVM release, builds
libc++/libc++abi/libunwind with `-DLLVM_USE_SANITIZER=MemoryWithOrigins`, and
installs to `$HOME/msan-libcxx`.

### 2. Install Rust nightly with rust-src

The Rust code is also compiled with MSan. This requires nightly (for
`-Zsanitizer=memory`) and rust-src (for `-Zbuild-std` to recompile std):

```bash
rustup install nightly-2026-02-12-x86_64-unknown-linux-gnu
rustup component add rust-src --toolchain nightly-2026-02-12-x86_64-unknown-linux-gnu
```

## Build

```bash
./scripts/build.sh -d build/llvm_msan -c clang \
  -f "-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -fsanitize-ignorelist=$(pwd)/.msan_ignorelist" \
  -t Debug \
  -x "-DMSAN_LIBCXX_PATH=$HOME/msan-libcxx" \
  -x "-DBUILD_BENCHMARKS=OFF" \
  -x "-DWITH_JEMALLOC=OFF"
```

Key flags:
- `-fsanitize=memory` — enables MSan
- `-fsanitize-memory-track-origins=2` — tracks where uninitialized values originate (invaluable for debugging)
- `-fsanitize-ignorelist=.msan_ignorelist` — compile-time ignorelist that skips instrumentation for our wrappers around uninstrumented libraries (OpenSSL, blake3, crc32c, SipHash)
- `-DMSAN_LIBCXX_PATH` — uses our instrumented libc++ instead of the system one
- `-DWITH_JEMALLOC=OFF` — MSan must intercept malloc/free; jemalloc bypasses this
- `-DBUILD_BENCHMARKS=OFF` — benchmarks aren't useful under sanitizers

The `MSAN_LIBCXX_PATH` flag also triggers Rust MSan: it switches to the nightly
toolchain and adds `-Zsanitizer=memory -Zbuild-std=std,panic_abort` so Rust's
std is recompiled with MSan instrumentation.

## Run tests

```bash
export MSAN_OPTIONS=halt_on_error=0:exitcode=0:print_stats=1:suppressions=$(pwd)/.msan_suppress:log_path=$(pwd)/msan_log
ctest --test-dir build/llvm_msan --parallel --timeout 100 --output-on-failure
```

Key MSAN_OPTIONS:
- `halt_on_error=0` — report but keep running (don't abort on first warning)
- `exitcode=0` — don't fail the test binary on MSan warnings
- `log_path=<path>` — write each report to `<path>.<pid>` files instead of stderr
- `suppressions=.msan_suppress` — runtime suppressions (limited effectiveness for uninstrumented code)

To run a single test binary directly:

```bash
MSAN_OPTIONS=halt_on_error=0:exitcode=0:log_path=$(pwd)/msan_log build/llvm_msan/hogd/hogdb/hogdb_test
```

## Collect and analyze results

If running on a remote server, copy the log files locally:

```bash
scp 'user@server:~/hogd/msan_log.*' msan/
```

(Quote the wildcard so your local shell doesn't expand it.)

Then filter out known false positives:

```bash
python3 scripts/msan_filter.py msan/
```

This suppresses ~99.9% of reports which are false positives from uninstrumented
OpenSSL. The script deduplicates remaining reports and prints only potentially
real bugs.

## Suppression layers

There are three layers for dealing with false positives:

| Layer | File | How it works | Scope |
|-------|------|-------------|-------|
| **Compile-time ignorelist** | `.msan_ignorelist` | Tells clang to skip MSan instrumentation for matching functions | C++ only (clang) |
| **MSAN_UNPOISON** | `hogd/crypto/crypto.h` | `__msan_unpoison()` calls mark output buffers as initialized at FFI boundaries | C++ functions wrapping uninstrumented libs |
| **Runtime suppressions** | `.msan_suppress` | Suppresses reports matching function name patterns | Unreliable for uninstrumented library frames |
| **Post-hoc filtering** | `scripts/msan_filter.py` | Filters log files by known false-positive patterns | Everything (last resort) |

The compile-time ignorelist and MSAN_UNPOISON handle taint propagation (preventing
cascading false positives). The post-hoc filter catches everything that slips through.

Runtime suppressions (`.msan_suppress`) have proven unreliable — MSan's suppression
matching doesn't work well for interceptor frames or uninstrumented shared library
symbols.

## Known limitations

- **OpenSSL is uninstrumented.** This produces ~99% of all reports (544k+ memcmp
  false positives in a typical run). Building OpenSSL from source with MSan would
  eliminate this but is non-trivial.
- **Rust compile-time ignorelist not supported.** Clang's `-fsanitize-ignorelist`
  doesn't apply to Rust code. wasmtime's `ValRaw` union false positive can only
  be suppressed via post-hoc filtering.
- **Signal handler conflicts.** wasmtime uses signal-based trap handling which can
  conflict with MSan's signal interceptors. Add `handle_segv=0:handle_sigbus=0`
  to MSAN_OPTIONS if runtime tests crash.

## Real bugs found

- **Uninitialized Uniquifier in authclaim_test.cc** — `SignedTransaction stx` was
  default-constructed without initializing `uniquifier` fields. Fixed by switching
  to value-initialized `Transaction tx{}`.
