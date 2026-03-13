2026-03-13

- Optimized `x23::detail::Serializer::putbytes()` in `x23/x23/serialize.h` to copy payload bytes first and then zero only the XDR padding bytes. The old path zeroed an overlapping 4-byte word before the copy for non-4-byte payload sizes, which added avoidable stores on every padded string or opaque byte vector.
- Added focused byte-serialization benchmarks in `x23/tests/bench.cc` to keep the work observable and compare aligned versus padded payload sizes.
- Benchmark result on `/tmp/hogd-codex-build/x23/tests/bench --benchmark_filter='serialize_bufview' --benchmark_min_time=1s`:
  `serialize_bufview<51>` improved from about `1.36 ns` to `0.86 ns` CPU time, while `serialize_bufview<48>` stayed effectively flat (`0.85 ns` to `0.84 ns`).
- Validation:
  `scripts/build.sh -d /tmp/hogd-codex-build`
  `/tmp/hogd-codex-build/x23/tests/test_xdr`
  `/tmp/hogd-codex-build/x23/tests/test_deserialize`
  `/tmp/hogd-codex-build/x23/tests/test_uiobuf`
