#!/bin/bash

SCRIPT_DIR="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
ROOT_DIR="$(realpath "${SCRIPT_DIR}/..")"

cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std
cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std --target wasm32-unknown-unknown
cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std --target riscv32im-unknown-none-elf
cd "${ROOT_DIR}/hogd/rust" && cargo fetch

set -ex

# The 0.9.0 tools depend on crates with rust-version 1.96, which is newer than
# the default toolchain on some CI images -- install with a pinned toolchain.
TOOL_RUST=1.96.0
rustup toolchain install "${TOOL_RUST}" --profile minimal --no-self-update

echo "Installing groundhog-cli..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://groundhog.dev/crates/api/v1/crates/ \
  cargo "+${TOOL_RUST}" install --locked --registry groundhog groundhog-cli@0.9.0

echo "Installing groundhog-rv32-lower..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://groundhog.dev/crates/api/v1/crates/ \
  cargo "+${TOOL_RUST}" install --locked --registry groundhog groundhog-rv32-lower@0.9.0
