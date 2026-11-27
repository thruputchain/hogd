# PointerTagSanitizer builds

[PointerTagSanitizer](https://github.com/Aarno-Labs/PointerTagSanitizer)
(PTSan) is Aarno Labs' memory-safety sanitizer. It stores an object ID in the
upper 16 bits of every pointer and checks each access against the bounds of
the object the pointer was derived from. Unlike AddressSanitizer, it reports
an access that leaves its own object and lands inside another live one. It
also reports use after free, double free, and invalid free. The
[paper](https://arxiv.org/abs/2607.19246) describes the design.

PTSan is not part of LLVM. It is a fork of LLVM, so a PTSan build of hogd
needs that fork's clang, lld, runtime, and libc++. The `HOGD_PTSAN` CMake
option adapts hogd to it. Only Linux on x86-64 has been tried.

## Building and running

Build the toolchain once. This clones PTSan 0.4.0 (LLVM 23.1.5), applies the
patches in `scripts/ptsan/patches`, and builds clang. It takes about 35
minutes on 4 cores and 7 GB of disk:

```sh
./scripts/ptsan/build-toolchain.sh
```

The source and build go to `~/.cache/hogd-ptsan`; `scripts/ptsan/env.sh` lists
the variables that move them. Then build hogd, or some of its targets, in
`build/ptsan`, and run tests as usual:

```sh
./scripts/ptsan/build.sh                      # everything
./scripts/ptsan/build.sh base_test hog-devnode
ctest --test-dir build/ptsan --output-on-failure
```

`build.sh` takes the options of `scripts/build.sh`. Compiling with PTSan takes
about 2.5 times as long as without, and the pass is slower still on large
coroutine code: `hog_devnode_node.cc` takes 13 minutes and 5 GB of memory.
The large test files need about 3.5 GB each, with or without PTSan, so choose
`-j` to fit. Building everything is about 11 hours of compile time, a little
under four hours with `-j3`, and 5 GB of disk.

A PTSan binary reports a bad access and exits with status 1:

```text
Found bad POINT access: #0. Function ID: 0x9133dff3fcddeffe, Ptr ID: 0x4a37
		Base: 0x559f062ee2d0, Bound: 0x559f062ee2e0 (Alloc size  0x10)
		Min:  0x559f062ee2e0, Max:   0x559f062ee2e1 (Access size 0x1),
		Min-Base = 16, Span-Size = 15
```

`Base` and `Bound` delimit the object the pointer belongs to, `Min` and `Max`
the access. An `Alloc size` of 0 means the object was already freed. The
report names the function only by a hash and has no stack trace, so rerun the
program under `scripts/ptsan/backtrace.sh`, which stops in gdb at the report:

```sh
./scripts/ptsan/backtrace.sh build/ptsan/hogd/base/base_test --gtest_filter='Rc.*'
```

The same script shows where a `SIGSEGV` comes from. Under PTSan that is
usually not a wild pointer in hogd but a valid, tagged pointer that reached
code PTSan did not instrument; see the next section.

## What `HOGD_PTSAN` changes

`cmake/ptsan.cmake` holds the build changes, each with its reason. In short:

- Everything is compiled and linked with `--config=ptsan-glibc.cfg`, which the
  toolchain build generates. It turns on `-fsanitize=ptsan` and selects the
  PTSan libc++.
- Debug builds use `-O1`, because PTSan does not support `-O0`.
- Interprocedural optimization is off. PTSan 0.4.0 fails under both ThinLTO
  and full LTO.
- BLAKE3 uses its C intrinsics backend rather than assembly, and RocksDB is
  built without jemalloc, liburing, and compression. The indexer therefore
  stores its database uncompressed. A PTSan binary has not been tried on a
  database that a regular build wrote; RocksDB without LZ4 should refuse it.

Code that the PTSan compiler did not build cannot be handed a tagged pointer,
and memory it allocates must not come back tagged. hogd calls three such
bodies of code, each through shims in `hogd/ptsan`; `shim.h` there explains
the mechanism.

| Uninstrumented code | Shims | Covers |
|---|---|---|
| Rust crypto crate | `rust_shims.cc` | every exported function |
| System libcrypto | `openssl_shims.cc` | the functions hogd calls |
| glibc functions PTSan does not wrap | `libc_shims.cc` | the ones hogd, x23, and libc++ have run into |

A call into uninstrumented code that has no shim still links. At run time it
faults inside the library, or, for a system call, fails with `EFAULT`. When
hogd starts calling a new libcrypto function, or a libc function PTSan does
not know, add a shim. `libc_shims.cc` says how to list the libc candidates.

## Limits

With 16-bit IDs PTSan tracks at most 65,535 live objects. Beyond that, new
objects share one fallback ID and are not checked. An idle `hog-devnode` that
has run a handful of transactions holds about 23,000; a node under load has
not been measured. `-DHOGD_PTSAN_CONFIG=ptsan-glibc-id32.cfg` selects PTSan's
32-bit IDs instead, but hogd has not been built that way, and the first patch
below does not cover it.

`hogd::TaggedPointer` keeps its tag in the pointer bits PTSan uses, so the two
cannot be combined. Nothing but the class's own test uses it, and that test
is skipped under PTSan.

The test FFI library (`ENABLE_TEST_FFI`) and the benchmarks have not been
built with PTSan.

## PTSan 0.4.0 defects

hogd runs into these. The first two are fixed by patches to the toolchain,
since nothing in hogd can avoid them. The rest are worked around in the tree,
and each workaround can go once a PTSan release fixes the defect.

- PTSan reports valid accesses of the form `begin[(end - begin) - 1]`, which
  is how `x23::Vec::back()` reads its last element. The optimizer turns the
  address into a GEP whose base is the literal -1, and PTSan takes the
  object ID from that literal.
  (`scripts/ptsan/patches/0001-ptsan-literal-gep-base.patch`)
- Compilation stops with `Size should not be negative!` at a `memcpy` or
  `memset` whose constant length has the top bit set. `v.resize(v.size() - 4)`
  on a `std::vector` inlines to one.
  (`scripts/ptsan/patches/0002-ptsan-huge-constant-size.patch`)
- A `thread_local` object with a destructor crashes at first use, because
  glibc's `__cxa_thread_atexit_impl` gets tagged memory from PTSan's
  allocator. `fdopendir` fails the same way, and with it
  `std::filesystem::remove_all`. (`libc_shims.cc`)
- `fchmodat`, `symlink`, `faccessat`, `fread_unlocked`, `madvise`,
  `CPU_COUNT`, and the pointer argument of `fcntl` reach glibc still tagged.
  So `std::filesystem::permissions` and `create_symlink` fail with `EFAULT`,
  and RocksDB can neither lock its database directory nor read its files.
  (`libc_shims.cc`)
