# Repository Guidelines

## Project Structure & Module Organization

Core C++ source lives under `hogd/` by subsystem (`auth`, `hogexec`, `mempool`,
`ramdb`, etc.). Rust tooling and deployment utilities live under `hogx/`, with a
workspace in `hogx/Cargo.toml`; Rust libraries used by C++ are under
`hogd/rust/`. Protocol XDR files are in `protocol/`, contract assets are in
`built_contracts/`, and design notes are in `docs/superpowers/specs/`.
Experimental RISC-V work follows the docs in `docs/rv32/` and lives in
`hogd/hogexec/runtime/rv32/`; keep it isolated from production WASM. Vendored
code is under `external/`; avoid editing it unless intentional.

## Build, Test, and Development Commands

- `./scripts/macos-setup.sh`: install macOS dependencies.
- `source ./scripts/macos-env.sh`: activate macOS build env.
- `git submodule update --init --recursive`: fetch submodules.
- `./scripts/fetch.sh`: fetch Rust crates.
- `./scripts/build.sh -n -d build -t Debug -c clang`: configure Debug clang.
- `./scripts/build.sh -d build -t Release -c gcc`: build.
- `ctest --test-dir build --output-on-failure`: run CTest tests.
- `./docker/local-ci.sh cpp`: run containerized C++ CI locally.

## Coding Style & Naming Conventions

C++ uses C++23 and the root `.clang-format` style, based on LLVM with custom
brace wrapping, 2-space indentation, and right-aligned pointers. Match nearby
code before introducing a new pattern. Function definitions put the return type
on its own line; control-flow braces stay on the same line. Use `PascalCase` for
types, `snake_case` for functions, `kCamelCase` for constants, and trailing
underscores for members. Group includes as standard library, POSIX/system, then
project headers. Rust uses `rustfmt` with `max_width=80`; shell uses
`shfmt -i 2`. Run `./scripts/format.sh` before submitting, or
`./scripts/format.sh -c` to check.

## Testing Guidelines

C++ tests use GoogleTest, are registered with `gtest_discover_tests`, and run
through CTest. Add tests next to the changed subsystem, for example
`hogd/hogexec/runtime/..._test.cc`, and update that directory's `CMakeLists.txt`
for new targets. For Rust under `hogx/`, run `cargo test --manifest-path ...`.

## Commit & Pull Request Guidelines

Recent commits use imperative summaries with optional prefixes such as `fix:`,
`test:`, `perf:`, or `mempool:`. Keep the first line focused and add PR or issue
context in the body when helpful. Pull requests should describe the
behavior change, list tests run, call out protocol or config impacts, and link
the relevant issue or design note.

## Security & Configuration Tips

Do not commit Cargo registry tokens, local `scripts/conf.sh`, build artifacts,
or generated secrets. Configure the `groundhog` registry as documented in
`README.md`.

## Agent-Specific Instructions

For questions about external libraries, SDKs, CLIs, or cloud services, fetch
docs with Context7 before answering or changing usage.
