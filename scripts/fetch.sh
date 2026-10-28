#!/bin/bash

SCRIPT_DIR="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
ROOT_DIR="$(realpath "${SCRIPT_DIR}/..")"

cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std
cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std --target riscv32im-unknown-none-elf
cd "${ROOT_DIR}/hogd/rust" && cargo fetch

set -ex

# The release tools depend on crates with rust-version 1.96, which is newer than
# the default toolchain on some CI images -- install with a pinned toolchain.
TOOL_RUST=1.96.0
rustup toolchain install "${TOOL_RUST}" --profile minimal --no-self-update

# The CLI is `thruput` on crates.io. Keep this version in step with the gates
# in built_contracts/build_risc.sh and hogd/loadgen/hackathon/build_contracts.py.
echo "Installing the thruput CLI..."
cargo "+${TOOL_RUST}" install --locked thruput@0.42.0
