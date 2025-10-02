Build Status : ![build status](https://github.com/groundhogchain/hogd/actions/workflows/cpp.yml/badge.svg)

# Installation Instructions

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

