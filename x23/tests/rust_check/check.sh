#!/bin/bash
# Usage: check.sh path/to/x23c
#
# Generates Rust from check.x the way generated crates do, and from plain.x
# with every derive feature suppressed, then type-checks them together with
# the uses in src/lib.rs, then runs their tests.

set -euo pipefail

x23c=$(realpath "$1")
cd "$(dirname "$0")"
"$x23c" --rust --rust-feature xdrs --rust-feature schema \
  --rust-feature spec -o src/generated.rs check.x
"$x23c" --rust --rust-no-feature xdrs --rust-no-feature spec \
  --rust-no-feature schema --rust-no-feature serde -o src/plain.rs plain.x
cargo check --locked --all-targets --all-features
cargo test --locked --all-features
