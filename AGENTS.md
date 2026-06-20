# AGENTS.md

## Cursor Cloud specific instructions

`hogd` (groundhogchain) is a C++23 + Rust blockchain validator/daemon. The build is
driven by CMake (`scripts/build.sh`, `scripts/check.sh`) and pulls in both public
git submodules and several **private** dependencies.

### Toolchain (already provisioned in the VM snapshot)

The following are installed system-wide and do **not** belong in the update script:

- `clang-21` / `clang++-21` (default `clang`/`clang++` via update-alternatives), `lld-21`,
  `libc++-21`, `clang-format-21`, `clang-tidy-21`, plus the `llvm-*-21` tools.
- `gcc-15` / `g++-15` (from the `ubuntu-toolchain-r/test` PPA; required for
  `__attribute__((musttail))` in the rv32 interpreter and C++23 features).
- Rust via rustup in `/usr/local/cargo`: **default toolchain `1.93.0`** (pinned by
  `hogd/rust/rust-toolchain.toml`) and `nightly-2026-02-12` with `rust-src` and the
  `wasm32-unknown-unknown` + `riscv32im-unknown-none-elf` targets (for
  `built_contracts/`). `cbindgen` 0.29.2 is installed.
- `wabt` (`wat2wasm`), `binaryen` (`wasm-opt`), `shfmt`, `ccache`, `python3-yaml`,
  `python3-flake8`, and the RocksDB system deps (`liblz4`, `libsnappy`, `libzstd`,
  `libbz2`, `zlib`, `libjemalloc`, `libssl`).

The `.devcontainer/Dockerfile` is the source of truth for the toolchain (it targets
Ubuntu 26.04 / clang-21 / gcc-15); here the same toolchain is layered onto Ubuntu 24.04.

### Credentials

1. **Private Cargo registry — WORKING.** The Rust workspace (`hogd/rust`) and
   `built_contracts/` depend on crates from the private `groundhog` registry
   (`hogexec-internal`, `groundhog-xdr`, `groundhog-wasm-meter`, `groundhog-cli`, `xdrs`, …).
   `CARGO_REGISTRIES_GROUNDHOG_TOKEN` is provided as a secret and
   `CARGO_REGISTRY_GLOBAL_CREDENTIAL_PROVIDERS=cargo:token` is set in the environment, so
   `cargo build`/`test`/`fetch` work directly in `hogd/rust`. (If you ever need to set it
   manually: `export CARGO_REGISTRY_GLOBAL_CREDENTIAL_PROVIDERS=cargo:token`.)

2. **Private git submodules — still a blocker for the C++ build.** `protocol`, `hogx`,
   `groundhog-go`, and `built_contracts/integration_test_contracts` live in the private
   `groundhogchain` GitHub org and `.gitmodules` references them via `git@github.com:`
   (SSH). Without the `protocol` submodule the C++ build fails at
   `protocol/syscalls.yaml` (it supplies all the XDR `.x` interface definitions that
   `hogd/xdr` and nearly everything else depends on). The public submodules
   (`external/*`) are initialized by the update script.

   The cloud agent shell has **no GitHub credential that can read these private repos**
   (the default installation token is scoped to `hogd`, and there is no git credential
   helper for `github.com`). The update script will auto-fetch the private submodules
   **if a read-scoped PAT is provided as the secret `GH_PRIVATE_SUBMODULE_TOKEN`** (it
   rewrites the `git@github.com:` URLs to token-authenticated HTTPS). To fetch them
   manually with a token `$T` that has access:
   ```sh
   git -c url."https://x-access-token:${T}@github.com/".insteadOf="git@github.com:" \
       submodule update --init --recursive protocol hogx groundhog-go \
       built_contracts/integration_test_contracts
   ```
   Then `./scripts/fetch.sh` fetches the remaining Rust deps (it also installs
   `groundhog-cli` / `groundhog-rv32-lower` from the registry for `built_contracts/`).

Once the private submodules are present, build the daemon with
`./scripts/build.sh -c clang` (see the build notes below) and run tests via
`make -C <builddir> test` or `./scripts/check.sh -p -s`.

### Building / testing / running

Standard commands are documented in `README.md` and `docker/README.md`; non-obvious points:

- `scripts/build.sh -c clang` uses the clang-21 default. **`-c gcc` resolves to plain
  `gcc` (v13), which is too old for C++23** — for a gcc build, create `scripts/conf.sh`
  pinning `GCC_CC=gcc-15` / `GCC_CXX=g++-15` (and `CLANG_CC=/usr/bin/clang-21`,
  `CLANG_CXX=/usr/bin/clang++-21`), exactly as `scripts/dev.sh` does. `scripts/env.sh`
  auto-sources `scripts/conf.sh` if present (it is git-ignored).
- CMake **configure succeeds without the private deps** (`scripts/build.sh -n ...`);
  the failure only appears at `make` time, so a clean configure is not evidence of a
  working build.
- Full local CI: `scripts/check.sh -p -s` (format + build + test). Tidy uses
  `scripts/tidy-ubuntu.sh`. Tests run via `ctest`/`make -C <builddir> test`.
- Lint: `python3 -m flake8` (the bare `flake8` command is not on PATH — `tidy-ubuntu.sh`
  already uses `python3 -m flake8`); `cargo clippy` for Rust; `clang-tidy` for C++.
- `scripts/dev.sh` and `docker/local-ci.sh` run everything in a container; on the cloud
  VM the toolchain is already native, so the `scripts/*` can be run directly.
