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
make -C build -j$(nproc) hog_devnode
export GROUNDHOG_DEVNODE_BINARY="$PWD/build/hogd/devnode/hog_devnode"
groundhog node
~~~~

`hog-devnode` itself serves only hogd's internal x23 RPC on loopback and prints
one readiness line on stdout. The public JSON-RPC endpoint that clients and the
CLI talk to (port 9545 by default) is served by `groundhog node`, which
supervises the devnode process. Run `hog_devnode --help` for its options.

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
