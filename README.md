Build Status : ![build status](https://github.com/groundhogchain/hogd/actions/workflows/cpp.yml/badge.svg)

# Installation Instructions

## macOS Quick Start

For macOS users, an automated setup script is available that installs all dependencies:

1. Install [Homebrew](https://brew.sh/) if you haven't already.

2. Run the macOS setup script:
   ~~~~ .sh
   ./scripts/macos-setup.sh
   ~~~~

3. Follow the normal build instructions below (starting from step 5).

The setup script installs all required dependencies including LLVM, CMake, Python packages, Rust toolchains, and development tools.

**Note:** An SSH agent must be running to fetch private submodules. Configure your SSH keys with GitHub before running the build.

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

See `docker/README.md` for detailed usage, including SSH agent setup for private submodules, tidy checks, and more.

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

1. Configure cargo to authenticate with the groundhog crate registry.
   First, ensure `~/.cargo/config.toml` includes:

    ~~~~ .toml
    [registry]
    global-credential-providers = ["cargo:token"]
    ~~~~

   Then add your token to `~/.cargo/credentials.toml`:

    ~~~~ .toml
    [registries.groundhog]
    token = "Bearer <your-token>"
    ~~~~

   You can get an access token by logging in at
   [crates.thruput.network](https://crates.thruput.network).

   Alternatively, you can authenticate using environment variables (this
   is how CI is configured):

    ~~~~ .sh
    export CARGO_REGISTRIES_GROUNDHOG_TOKEN="<your-token>"
    export CARGO_REGISTRY_GLOBAL_CREDENTIAL_PROVIDERS="cargo:token"
    ~~~~

1. Fetch rust crate dependencies.
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

## Compiler caches

Install `ccache` for C/C++ and `sccache` for native Rust builds. On Arch Linux:

```sh
sudo pacman -S --needed ccache sccache
```

The build detects sccache automatically and sets `CARGO_INCREMENTAL=0` for native
Rust compilation and workspace tests. Machines with a `10.100.*` interface use
the shared WebDAV cache at `http://hog12:9281/`, under `hogd/rust/`. The service
accepts only clients in `10.100.0.0/16` and requires no password. Outside that
network, sccache uses its normal configuration/local cache. Explicit sccache
backend environment settings or `SCCACHE_CONF` take precedence over discovery.

To select a different WebDAV cache, set `SCCACHE_WEBDAV_ENDPOINT` and optionally
`SCCACHE_WEBDAV_KEY_PREFIX` before configuring. Set
`HOGD_SCCACHE_REMOTE=0` to skip cluster discovery, or pass
`-x -DHOGD_USE_SCCACHE=OFF` to `scripts/build.sh` to disable Rust caching.
After changing storage settings, stop the idle daemon with
`sccache --stop-server` so its next invocation loads the new configuration.

```sh
./scripts/build.sh -d /tmp/gcc -x -DBUILD_BENCHMARKS=ON
ccache --show-stats -vv
sccache --show-stats
```

The Namespace Linux runner image includes checksum-verified sccache 0.18.0, and
jobs persist compiled Rust artifacts in `~/.cache/sccache`. Rebuild the runner
image from `.devcontainer/Dockerfile` when updating the pinned version. CI setup
downloads the pinned binary only as a fallback for hosts or older images that
do not already provide it. Private-cluster warmup jobs use an isolated daemon
to populate WebDAV directly, report both compiler caches even when a build fails,
and stop their daemon on exit without affecting other builds. Existing Cargo
outputs can skip compilation entirely; a fresh output directory is needed to
measure compiler cache reuse. RV32 contract builds retain their separate Cargo
cache and currently clear `RUSTC_WRAPPER`.

For standalone Cargo commands, use `RUSTC_WRAPPER=sccache` and source
`scripts/sccache-env.sh` to apply the same incremental and storage settings.

## Building `hog-devnode` for contract development

Contract authors normally do not build this repository. The `groundhog` CLI
(from [groundhog-rpc](https://github.com/groundhogchain/groundhog-rpc)) drives
a local node through the `hog-devnode` binary, which ships in the
`groundhog-client` release archive; see
[Develop Locally](https://docs.groundhog.dev/tutorials/develop-locally/) on the
developer docs site.

If you do want to build `hog-devnode` from source, configure as above and build
just that target:

~~~~ .sh
make devnode
export GROUNDHOG_DEVNODE_BINARY="$PWD/build/hogd/devnode/hog-devnode"
groundhog node
~~~~

`hog-devnode` itself serves only hogd's internal x23 RPC on loopback and prints
one readiness line on stdout. The public JSON-RPC endpoint that clients and the
CLI talk to (port 9545 by default) is served by `groundhog node`, which
supervises the devnode process. Run `hog-devnode --help` for its options.
Fresh local chains allow up to 256 KiB of RV32 code and 512 KiB per complete
contract artifact by default; no network-config overrides are needed.

Fresh local and hosted bootstrap accounts use the native default wallet at
nonce zero. Genesis funds their public-key-derived addresses directly; runtime
bundles need `genesis_root.rv32a` but no `dev_wallet.rv32a`. Bootstrap and runtime
manifests use schema 3; hosted bootstrap metadata uses schema 2.

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
