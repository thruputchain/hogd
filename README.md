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

1. Install the `wat2wasm` utility from the [WebAssembly Binary
   Toolkit](https://github.com/WebAssembly/wabt).  Most OS package
   managers let you do this by installing a package called `wabt`.

1. Install the `wasm-opt` utility from [Binaryen](https://github.com/WebAssembly/binaryen).
   Most package managers let you do this by installing the `binaryen` package.

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
   packages installed, because they won't support WASM.  You can
   install rustup with:

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
    global-credential-providers = ["env", "cargo:token"]
    ~~~~

   Then add your token to `~/.cargo/credentials.toml`:

    ~~~~ .toml
    [registries.groundhog]
    token = "Bearer <your-token>"
    ~~~~

   You can get an access token by logging in at
   [groundhog.dev/crates](https://groundhog.dev/crates).

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
