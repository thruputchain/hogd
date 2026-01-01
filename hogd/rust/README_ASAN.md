# To run tests with asan on macos:
`export RUSTFLAGS=-Zsanitizer=address`
Then
`cargo +nightly test --target=aarch64-apple-darwin`

For (unclear to me) reasons this changes the way that proc-macro crates are compiled.
Without the --target flag, compilation with asan fails on macos with inexplicable errors
mentioning DYLD.

See
- https://doc.rust-lang.org/beta/unstable-book/compiler-flags/sanitizer.html
- https://github.com/rust-lang/rust/issues/63986
- https://github.com/rust-lang/rust/issues/79934


