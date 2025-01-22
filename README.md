Build Status : ![build status](https://github.com/groundhogchain/hogd/actions/workflows/cpp.yml/badge.svg)

# Installation Instructions

1. Install the `wat2wasm` utility from the [WebAssembly Binary
   Toolkit](https://github.com/WebAssembly/wabt).  Most OS package
   managers led you do this by installing a package called `wabt`.

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

1. Configure the build system using "`cmake -B `_builddir_", setting
   the `CXX` and `CXXFLAGS` environment variables if you don't want to
   use the default.  For instance, any of the following might be what
   you want:

    * `cmake -B build`
    * `CXXFLAGS=-ggdb cmake -B debug-build`
    * `CXX=clang++ cmake -B clang-build`

1. Build the system by running `make` in the build directory, E.g.:

    * `cd build && make`
    * `make -C build -j$(nproc)`
