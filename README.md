Build Status : ![build status](https://github.com/thruputchain/hogd/actions/workflows/cpp-namespace-optimized.yml/badge.svg)

# hogd

Thruput's core execution engine and node runtime.

[Website](https://www.thruput.network/) ·
[Developer docs](https://www.thruput.network/docs/) ·
[Community discussions](https://github.com/thruputchain/thruput-docs/discussions) ·
[Discord: #thruput](https://discord.com/channels/897514728459468821/1564948118162898954)
([join server](https://discord.gg/stellardev))

## macOS Quick Start

For macOS users, an automated setup script is available that installs all dependencies:

1. Install [Homebrew](https://brew.sh/) if you haven't already.

2. Run the macOS setup script:
   ~~~~ .sh
   ./scripts/macos-setup.sh
   ~~~~

3. Follow the normal build instructions below (starting from step 5).

The setup script installs all required dependencies including LLVM, CMake, Python packages, Rust toolchains, and development tools.

## Docker

For development and testing in a containerized environment, use the Docker-based CI setup:

~~~~ .sh
# Build the Docker image
./docker/local-ci.sh build

# Run C++ CI checks (format, build, test)
./docker/local-ci.sh cpp

# Interactive shell for debugging
./docker/local-ci.sh shell
~~~~

See `docker/README.md` for detailed usage, including tidy checks and image builds.

## Manual Installation (All Platforms)

1. Install python-yaml, either with your OS package manager (e.g.,
   `pacman -S python-yaml`) or with `pip install pyyaml`.

1. If you plan to use clang as your compiler, install the llvm package as well.

    ~~~~ .sh
    sudo pacman -S llvm libc++
    ~~~~

1. Pull all of the submodules.

    ~~~~ .sh
    git submodule update --init --recursive
    ~~~~

1. Install [`rustup`](https://rustup.rs/) if you haven't already.  It
   generally won't suffice to have your OS distribution's rust
   packages installed, because the contract fixtures need a pinned
   nightly toolchain with the `riscv32im-unknown-none-elf` target.
   You can install rustup with:

    ~~~~ .sh
    curl -sSf https://sh.rustup.rs | sh
    ~~~~

1. Install cbindgen.

    ~~~~ .sh
    cargo install --force cbindgen
    ~~~~

1. Fetch rust crate dependencies. They all come from crates.io, so this needs
   no registry credentials.
    ~~~~ .sh
    ./scripts/fetch.sh
    ~~~~

1. Use cmake to configure the build, or run
    ~~~~ .sh
    ./scripts/build.sh -n
    ~~~~
    to get the defaults, or to specify the directory, build type
    (`Debug` or `Release` [default]) and compiler (`gcc` [default] or
    `clang`) explicitly:
    ~~~~ .sh
    ./scripts/build.sh -n -d build -t Debug -c clang
    ~~~~

1. Build the system by running `make` in the build directory, E.g.:

    * `cd build && make`
    * `make -C build -j$(nproc)`

1. Tidy and format scripts require the following:
    * clang-format
    * clang-tidy
    * clippy (for rust)
    * flake8 (for python)
    * shfmt

## Building `hog-devnode` for contract development

Contract authors normally do not build this repository. The `thruput` CLI
(from [thruput-tools](https://github.com/thruputchain/thruput-tools)) drives
a local node through the `hog-devnode` binary, which ships in the
`thruput-client` release archive; run `thruput docs` to browse the bundled
developer documentation.

If you do want to build `hog-devnode` from source, configure as above and build
just that target:

~~~~ .sh
make devnode
export THRUPUT_DEVNODE_BINARY="$PWD/build/hogd/devnode/hog-devnode"
thruput node
~~~~

`hog-devnode` itself serves only hogd's internal x23 RPC on loopback and prints
one readiness line on stdout. The public JSON-RPC endpoint that clients and the
CLI talk to (port 9545 by default) is served by `thruput node`, which
supervises the devnode process. Run `hog-devnode --help` for its options.
Fresh local chains allow up to 256 KiB of RV32 code and 512 KiB per complete
contract artifact by default; no network-config overrides are needed.

Fresh local and hosted bootstrap accounts use the native default wallet at
nonce zero. Genesis funds their public-key-derived addresses directly; runtime
bundles need only `genesis_root.rv32a`. Bootstrap and runtime manifests use
schema 5; hosted bootstrap metadata uses schema 3.

Persistent local directories from the older dev-wallet bootstrap are not migrated
in place. Keep the original CLI/runtime to open those directories, or explicitly
choose a new data directory for a native-bootstrap chain. The new runtime refuses
old metadata before opening the database; do not delete a directory to resolve
that error unless you intend to discard its chain state. Deterministic signing
keys remain the same, but fresh native account addresses differ from the old
contract-wallet addresses.

## Update instructions

To update to the latest version in the upstream git repository, do the
following:

1. Update the main git repository:

    * `git pull`

2. Update all submodules

    * `git submodule update --init --recursive`

    Alternatively, put the following in `$HOME/.gitconfig`:

    ```
    [alias]
        sup = submodule update --init --recursive
    ```
    Then you can abbreviate the above command as

    * `git sup`

3. Fetch all the rust git repositories managed by cargo

    * `./scripts/fetch.sh`

## Repair instructions

If your rust installation gets messed up, delete the directories
`~/.cargo` and `~/.rustup`.  Then start over by reinstalling rustup.

```
curl -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
cargo install --force cbindgen
./scripts/fetch.sh
```

### AMD server release artifacts

Linux releases also publish `hogd-validator-linux-x86_64-znver5.tar.gz` in
`SHA256SUMS`. Its server binaries target AMD Zen 5 explicitly in both C++ and
embedded Rust (`BUILD_ARCHITECTURE=znver5`, `RUST_BUILD_CPU=znver5`). Use only
on qualified Zen 5 hosts, such as EC2 C8a/M8a. Packaging must select this archive
explicitly and qualify execution on the destination CPU before deployment.
The regular x86_64 archives and public hog-devnode distributions retain their
portable CPU baseline. There is no tuned standalone devnode distribution.

Run `python3 scripts/test_package_hogd_validator_release.py` to check archive
layout and profile validation. This fixture test does not validate machine code;
the release build and a real Zen 5 runtime smoke remain required.
