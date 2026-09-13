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

# groundhog-cli's published deps still resolve from the legacy index, which
# needs auth; name it so cargo can match a token to the URL.
export CARGO_REGISTRIES_GROUNDHOG_LEGACY_INDEX="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_INDEX:-sparse+https://groundhog.dev/crates/api/v1/crates/}"
# xtrace expands the token into the job log, so drop tracing here.
set +x
if [ -n "${CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN:-${CARGO_REGISTRIES_GROUNDHOG_TOKEN:-}}" ]; then
  export CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN:-${CARGO_REGISTRIES_GROUNDHOG_TOKEN}}"
  export CARGO_REGISTRIES_GROUNDHOG_LEGACY_CREDENTIAL_PROVIDER="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_CREDENTIAL_PROVIDER:-cargo:token}"
fi
set -x

echo "Installing groundhog-cli..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://crates.thruput.network/api/v1/crates/ \
  cargo "+${TOOL_RUST}" install --locked --registry groundhog groundhog-cli@0.17.0

echo "Installing groundhog-rv32-lower..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://crates.thruput.network/api/v1/crates/ \
  cargo "+${TOOL_RUST}" install --locked --registry groundhog groundhog-rv32-lower@0.17.0
