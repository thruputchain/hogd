#!/bin/bash

SCRIPT_DIR="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
ROOT_DIR="$(realpath "${SCRIPT_DIR}/..")"

cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std
cd "${ROOT_DIR}/built_contracts" && cargo fetch -Zbuild-std --target riscv32im-unknown-none-elf
cd "${ROOT_DIR}/hogd/rust" && cargo fetch

# loadgen_localhost builds this separate workspace after the C++ build.
# Fetch it before installing tools, while the installation token is fresh.
if [[ "${HOGD_FETCH_HOGX:-0}" == "1" ]]; then
  cargo fetch --locked --manifest-path "${ROOT_DIR}/hogx/Cargo.toml"
fi

set -ex

# The release tools depend on crates with rust-version 1.96, which is newer than
# the default toolchain on some CI images -- install with a pinned toolchain.
TOOL_RUST=1.96.0
rustup toolchain install "${TOOL_RUST}" --profile minimal --no-self-update

# thruput-cli's published deps still resolve from the legacy index, which
# needs auth; name it so cargo can match a token to the URL.
export CARGO_REGISTRIES_GROUNDHOG_LEGACY_INDEX="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_INDEX:-sparse+https://groundhog.dev/crates/api/v1/crates/}"
# xtrace expands the token into the job log, so drop tracing here.
set +x
if [ -n "${CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN:-${CARGO_REGISTRIES_GROUNDHOG_TOKEN:-}}" ]; then
  export CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN:-${CARGO_REGISTRIES_GROUNDHOG_TOKEN}}"
  export CARGO_REGISTRIES_GROUNDHOG_LEGACY_CREDENTIAL_PROVIDER="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_CREDENTIAL_PROVIDER:-cargo:token}"
fi
set -x

# thruput-cli and thruput-rv32-lower replace groundhog-cli and
# groundhog-rv32-lower. A cached ~/.cargo can still hold the old ones, and
# cargo install will not take over the `rv32-lower` binary that
# groundhog-rv32-lower owns there, so remove them first.
for legacy in groundhog-cli groundhog-rv32-lower; do
  if cargo "+${TOOL_RUST}" install --list | grep -q "^${legacy} v"; then
    cargo "+${TOOL_RUST}" uninstall "${legacy}"
  fi
done

echo "Installing thruput-cli..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://crates.thruput.network/api/v1/crates/ \
  cargo "+${TOOL_RUST}" install --locked --registry groundhog thruput-cli@0.30.0

echo "Installing thruput-rv32-lower..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://crates.thruput.network/api/v1/crates/ \
  cargo "+${TOOL_RUST}" install --locked --registry groundhog thruput-rv32-lower@0.30.0
