#!/bin/bash

SCRIPT_DIR="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
ROOT_DIR="$(realpath "${SCRIPT_DIR}/..")"

cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std
cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std --target wasm32-unknown-unknown
cd "${ROOT_DIR}/hogd/rust" && cargo fetch

set -ex

echo "Installing groundhog-cli..."
cargo install --index sparse+https://groundhog.dev/crates/api/v1/crates/ groundhog-cli@0.2.2
