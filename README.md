Build Status : ![build status](https://github.com/groundhogchain/hogd/actions/workflows/cpp.yml/badge.svg)

# Installation Instructions

1. Install the `wat2wasm` utility from the [WebAssembly Binary
   Toolkit](https://github.com/WebAssembly/wabt).  Most OS package
   managers let you do this by installing a package called `wabt`.

1. Install the `wasm-opt` utility from [Binaryen](https://github.com/WebAssembly/binaryen).
   Most package managers let you do this by installing the `binaryen` package.

1. Install python-yaml, either with your OS package manager (e.g.,
   `pacman -S python-yaml`) or with `pip install pyyaml`.

1. Install [`rustup`](https://rustup.rs/) if you haven't already.  It
   generally won't suffice to have your OS distribution's rust
   packages installed, because they won't support WASM.  You can
   install rustup with:

    ~~~~ .sh
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
    ~~~~

1. Install the WASM target for rust.

    ~~~~ .sh
    rustup +nightly target add wasm32-unknown-unknown
    ~~~~

1. Install the rust sources so that the hogd build process can
   recompile them in a more compact form to reduce code size.

    ~~~~ .sh
    rustup component add rust-src --toolchain nightly-x86_64-unknown-linux-gnu
    ~~~~

1. Install cbindgen.

    ~~~~ .sh
    cargo install --force cbindgen
    ~~~~

1. If you plan to use clang as your compiler, install the llvm package as well.

    ~~~~ .sh
    sudo pacman -S llvm
    ~~~~

1. Pull all of the submodules.

    ~~~~ .sh
    git submodule update --init --recursive
    ~~~~

1. Configure the build system using "`cmake -B `_builddir_", setting the
   `CXXFLAGS` environment variable if you don't want to use the default. Also
    set the `DCMAKE_C_COMPILER` and `DCMAKE_CXX_COMPILER` flags if you want to
    use clang. For instance, any of the following might be what you want:

    * `cmake -B build`
    * `CXXFLAGS=-ggdb cmake -B debug-build`
    * `cmake -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`

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

3. Update all the rust git repositories managed by cargo

    * `(cd built_contracts && CARGO_NET_GIT_FETCH_WITH_CLI=true cargo +nightly update)`

## Repair instructions

If your rust installation gets messed up, delete the directories
`~/.cargo` and `~/.rustup`.  Then start over by reinstalling rustup.

```
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup +nightly target add wasm32-unknown-unknown
rustup component add rust-src --toolchain nightly-x86_64-unknown-linux-gnu
cargo install --force cbindgen
export CARGO_NET_GIT_FETCH_WITH_CLI=true
(cd built_contracts && cargo +nightly update)
```
