# Installation Instructions

Tests require the `wat2wasm` utility, part of the [WebAssembly Binary Toolkit](https://github.com/WebAssembly/wabt).  Available in most package managers as `wabt`.

Relies on rust-nightly and on rebuilding core and alloc crates, which requires installing a rust-src package via rustup.  CI has example commands in ubuntu.
The following should install the correct components:
`rustup +nightly target install wasm32-unknown-unknown && rustup component add rust-src --toolchain nightly-x86_64-unknown-linux-gnu`
The SDK does not rely on many unstable features, but does require rebuilding core and alloc (this reduces the generated code size significantly)

