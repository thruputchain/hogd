#!/bin/bash
# Usage: check.sh path/to/x23c
#
# Generates Rust from check.x the way generated crates do, then type-checks
# it together with the uses in src/lib.rs.

set -euo pipefail

x23c=$(realpath "$1")
cd "$(dirname "$0")"
"$x23c" --rust --rust-feature xdrs --rust-feature schema \
  --rust-no-feature spec -o src/generated.rs check.x
cargo check --locked --all-targets --all-features
