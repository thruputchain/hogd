# AGENTS.md

This file provides guidance to LLM agents like Codex and Claude Code when working with code in this repository.

## What is x23

C++23 implementation of XDR (External Data Representation) and RPC (Remote Procedure Call). Includes:
- **x23c** — compiler that generates C++ headers (`.hh`), Rust (`.rs`), TypeScript (`.ts`) and C headers (`.h`) from `.x` XDR interface definitions
- **libx23** — async RPC runtime built on C++23 coroutines, epoll, and a work-stealing thread pool
- Used by the hogd project (Thruput)

## Build

Requires CMake 3.28+, Flex, Bison, and either GCC 14.2+ or Clang with libc++ 21.1+. Targets Linux.

### macOS development

AppleClang lacks the C++23 support this project needs. Use Homebrew LLVM (`brew install llvm lld bison`). Before building, set up the toolchain — the hogd repo has a convenience script, but the essential env vars are:

```bash
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:/opt/homebrew/opt/bison/bin:$PATH"
export CC="/opt/homebrew/opt/llvm/bin/clang"
export CXX="/opt/homebrew/opt/llvm/bin/clang++"
export LDFLAGS="-L/opt/homebrew/opt/llvm/lib/c++ -L/opt/homebrew/opt/llvm/lib"
export CPPFLAGS="-I/opt/homebrew/opt/llvm/include"
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/llvm"
```

Verify with `clang --version` — you need Homebrew clang (not Apple clang) before running cmake.

### Build commands

```bash
cmake -B build .
cmake --build build -j$(nproc)                    # build everything
cmake --build build --target test_rpcstress        # build one target
```

The build directory is `build/`. `compile_commands.json` is generated automatically.

## Tests

```bash
make -C build check                      # build + run all tests
ctest --test-dir build --output-on-failure --parallel  # run tests only
./build/tests/test_rpc                   # run a single test binary
./build/tests/test_rpcstress -s fanout -d 10 -t 4 -v  # stress test with options
```

Tests live in `tests/`. The `add_x23_test(name)` CMake macro creates `test_<name>` from `<name>.cc`, linked against libx23 and the xdrtest XDR library. GTest-based tests use `add_x23_gtest(name)` and require googletest (auto-detected from git submodules).

x23c's C output is checked by the `c_backend` test, which compares it with the C++ codec on random and corrupted encodings of the types in `tests/c_backend.x`; the `c_backend_c` target also compiles it as freestanding C11.

x23c's Rust output is checked outside ctest, since it needs cargo and crates.io: `tests/rust_check/check.sh build/x23c/x23c` generates Rust from `tests/rust_check/check.x` and runs `cargo check` on it. CI runs it (`x23c_rust_check.yml`) when x23c changes.

## Code generation

XDR files (`.x`) are compiled by x23c into `.hh` headers:
```
x23c -o output.hh input.x        # C++ header
x23c --rust -o output.rs input.x  # Rust
x23c --c -o output.h input.x      # C (types only; RPC programs are skipped)
```

CMake functions: `add_xdr(target files...)` creates an interface library from `.x` files (`add_xdr_c` likewise for C headers). `use_x23(target)` processes any `.x` files in the target's sources.

Generated headers define structs, enums, unions, and RPC program interfaces with `xdr_meta<T>` specializations for serialization.

C headers (`gen_c.cc`) need only `x23/xdr_c.h`, which needs only the freestanding headers: no libc, no allocator. Decoding is zero-copy where XDR allows it: `opaque<N>`/`string<N>` are views into the input; `T<N>`, `T*` and fields annotated `#[cpp(box)]` are views of their validated encoding (`T_vec`, `T_opt`, `T_box`) that decode elements on demand; everything else decodes to native fields. Enum constants are prefixed with their enum's name (`Color_RED`), since XDR scopes them. As in C++ and Rust, an enum decodes any int32; a bool union discriminant must be 0 or 1, as in Rust (C++ takes any nonzero value as TRUE). Decoding bounds view nesting with `X23_NESTING_LIMIT` instead of C++'s per-recursive-type limit. See the comment at the top of `x23/xdr_c.h`.

## Architecture

### Coroutine types (`x23/async.h`)
- **`Async<T>`** — awaitable coroutine return type. The primary async primitive.
- **`MaybeAsync<T>`** — returns `T` synchronously or `Async<T>`, avoiding coroutine frame allocation for fast paths.
- **`Detached`** — fire-and-forget coroutine. Returns before the coroutine body completes, so unnamed lambda coroutines with captures need `(this auto)` to copy the closure into the frame (the "most vexing capture").
- **`Awaitable<T>`** / **`CoroCB<T>`** — callback-driven future/promise pair.

### Threading (`x23/threadpool.h`, `x23/event.h`)
- **`ThreadPool`** — LLC-aware work-stealing pool. `co_await pool.bg()` yields to a worker thread.
- **`CoroPoll`** — epoll-based event loop driving async I/O. One instance per application.

### RPC (`x23/rpc.h`, `x23/srpc.h`)
- **`rpc_stream(cp, fd)` / `rpc_stream_logged(cp, fd)`** — create async RPC stream over a socket fd.
- **`tcp_rpc_server(cp, port, configurator)`** — listen on TCP, accept connections, invoke configurator to register server impls.
- Server implementations: struct with `using rpc_service = <version>;` and method signatures matching the XDR program. Validated with `static_assert(is_<version><Impl*, MaybeAsync>)`.
- Synchronous RPC available via `srpc.h`.

### Resource management (`x23/refcnt.h`)
- **`DestroyInhibit`** / **`RefPtr<DestroyInhibit>`** — prevents premature destruction while async work is in-flight. Pattern: create `DestroyInhibit di;`, pass `RefPtr{di}` into coroutines, call `di.destroy_wait()` at shutdown.
- **`RefCount<T>`** / **`RefPtr<T>`** — intrusive reference counting.

### XDR type system (`x23/x23.h`, `x23/serialize.h`)
- Types: `BoundedStr<N>`, `Vec<T,N>`, `Ptr<T>`, plus standard `std::array` for fixed arrays.
- Concepts: `is_xdr_type`, `is_xdr_struct`, `is_xdr_union`, `is_xdr_enum`.
- Serialization: `serialize(obj, buffer)` / `deserialize(&obj, buffer)`.

## Sanitizer builds

Use separate build directories for sanitizer configurations:
```bash
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -O0" .
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -O0" .
cmake -B build-o0 -DCMAKE_CXX_FLAGS="-O0" .
cmake --build build-tsan --target test_rpcstress -j$(nproc)
```

**Stale binary gotcha**: cmake dependency tracking sometimes misses header-only changes (especially to `x23/async.h`). If a rebuild completes instantly after editing a header, the binary is likely stale. Verify with `stat` timestamps and delete the binary to force a proper rebuild.

**`MALLOC_PERTURB_` for use-after-free**: When ASAN changes timing enough to suppress a race, `MALLOC_PERTURB_=85` (glibc fills freed memory with `0xAA`) can confirm use-after-free in the debugger without the overhead. On macOS, `MallocScribble=1` fills freed memory with `0x55`.

## Shutdown and destruction ordering

**`CoroPollImpl` holds a bare `ThreadPool&` reference** (not `RefPtr`). ThreadPool must outlive CoroPoll — destroying ThreadPool first causes use-after-free when `~CoroPoll()` calls `reset(fd)` → `docbs()` → `pool_.make_callback()`.

Correct shutdown sequence (matches production `RpcServer` pattern):
1. Signal stop (`stop_source.request_stop()`)
2. Poison listen fd (`cp->poison(lfd)`) — stops accepting new connections
3. Walk connections and `fail()` each — tears down in-flight RPCs
4. `destroy_wait()` — waits for all `RefPtr<DestroyInhibit>` refs to drop
5. Destroy CoroPoll (`cp.reset()`)
6. Destroy ThreadPool (`pool.reset()`)

## Coroutine internals — known pitfalls

**Suspend-point index race**: The compiler writes a 2-byte suspend-point index to the coroutine frame *between* `final_suspend()` returning its awaiter and `await_suspend()` being called on that awaiter. Any state publication in `final_suspend()` that allows another thread to destroy the coroutine frame creates a data race with this write. The fix pattern: defer publication to `await_suspend()`, which runs after the suspend-point write. See `Awaitable<T>`'s `TailCoro` and issue #1022.

**HALO (Heap Allocation Elision Optimization)**: At `-O2`, the compiler can elide coroutine frame heap allocations entirely, inlining them into the caller's frame. Bugs involving coroutine frame lifetime often only manifest at `-O0` (where every frame is heap-allocated) or under sanitizers (TSAN, ASAN). Always test coroutine changes at `-O0` with high thread counts.

## Code style

This is a bespoke, handcrafted, performance-critical C++23 codebase. Match the existing style exactly — do not modernize, reformat, or "improve" patterns that are already established. When in doubt, read the surrounding code and do what it does.

### Formatting
- Return type on its own line for function definitions (K&R style):
  ```cpp
  void
  set_nodelay(BorrowedFd fd, bool on)
  {
  ```
- Opening brace on same line for `if`/`for`/`while`/`switch`/lambdas, own line for function definitions.
- 2-space indentation. No tabs.
- `// -*-C++-*-` modeline at top of headers, then blank line, then `#pragma once`.
- Standard library includes sorted alphabetically, then blank line, then POSIX includes, then blank line, then project includes (`<x23/...>` in headers, `"x23/..."` in .cc files).

### Naming
- Types: `PascalCase` (`ThreadPool`, `UniqueFd`, `HostPort`)
- Functions: `snake_case` (`set_nonblock`, `tcp_rpc_server`, `show_addr`)
- Constants: `kCamelCase` (`kInvalidFd`, `kDegree`, `kMaxXdrSize`)
- Member variables: trailing underscore (`fd_`, `pool_`, `elems_`)
- Template parameters: `PascalCase` (`T`, `Action`, `Degree`)

### CTAD
Prefer compiler type deduction over explicit template arguments. Getting explicit types wrong can silently compile but do something unexpected (slicing, implicit conversions). Use CTAD when the compiler can deduce correctly; only spell out template arguments when deduction would give the wrong type (e.g. `std::atomic<uint64_t>` where deduction would give `int`) or when declaring parameter types (where CTAD doesn't apply).

### Patterns
- `using namespace x23;` and `using namespace std::chrono;` in .cc files and tests — no prefix ceremony.
- `namespace x23 { ... } // namespace x23` — closing comment on namespace braces.
- `constexpr inline` for namespace-scope constants.
- `explicit operator bool()` for nullable/validity checks.
- RAII everywhere: `UniqueFd`, `ManagedPtr<Destructor>`, `Defer`, `DestroyInhibit`.
- `std::expected<T, Error>` for fallible operations, exceptions (`Errno{}.raise(...)`) for unrecoverable errors.
- Concepts for constraining templates, `static_assert` for validating RPC server implementations.
- `deducing this` (`this auto &&self`, `this RefPtr<T> self`) for methods that need the object's exact type or reference-counted self.
- `(this auto)` on unnamed coroutine lambdas with captures. The coroutine returns (e.g. `Detached{}`) before the body completes, so the closure is a temporary destroyed before the coroutine finishes. `(this auto)` copies the closure into the coroutine frame so captures survive. Captureless lambdas are equivalent to function pointers and don't need this. Named closures that outlive the coroutine don't either.
- `#if NDEBUG` / `#else` guards for debug-only code (not `#ifdef`).
- No unnecessary abstractions. Three similar lines > a premature helper function.
- Comments explain *why*, not *what*. Brief, direct, no docstring boilerplate.

### RPC debug logging
`#if NDEBUG` switches between `rpc_stream` (release) and `rpc_stream_logged` (debug) via macro. Set `X23TRACE=2` env var to enable per-connection trace output to stderr. For `tcp_rpc_server`, use `X23SERVER_TRACE` or `X23SERVER_TRACE_<progname>`.

### Lint before PR

Format and tidy checks run in CI (the "Tidy" workflow). Run locally before pushing:

```bash
# Format check (dry-run; drop --dry-run to fix in place)
find x23 tests -name '*.h' -o -name '*.cc' | xargs clang-format-21 --dry-run -Werror

# Tidy (needs compile_commands.json from a build directory)
find x23 tests -name '*.h' -o -name '*.cc' | \
  xargs run-clang-tidy -j$(nproc) -use-color -p build \
    -header-filter='.*' -quiet
```

Config lives in the repo root: `.clang-format` and `.clang-tidy`. Tidy treats all warnings as errors (`WarningsAsErrors: '*'`). Some pre-existing warnings in test files (e.g. `google-build-using-namespace`, `misc-use-internal-linkage` for test globals) match the established pattern and are not CI failures — CI only runs tidy on `hogd/` sources, not the `x23/` subtree directly.
