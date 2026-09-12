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

# The crates themselves now come from crates.thruput.network, but the published
# groundhog-cli/groundhog-rv32-lower releases record their own dependencies
# against the legacy registry URL, so cargo still resolves those deps from
# groundhog.dev. That index needs auth, and cargo only finds a token for it by
# matching the index URL against a named registry -- with no such name
# configured the install dies with:
#
#   error: no token found for `sparse+https://groundhog.dev/crates/api/v1/crates/`
#
# Name the legacy index here and reuse the groundhog token for it (the two
# registries share a credential), so the installs authenticate wherever this
# script runs. The workflows export these directly for jobs that run cargo on
# the GitHub runner; hog_sched jobs only inherit the env their schedule
# declares, which is why deriving them here matters.
export CARGO_REGISTRIES_GROUNDHOG_LEGACY_INDEX="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_INDEX:-sparse+https://groundhog.dev/crates/api/v1/crates/}"
if [ -n "${CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN:-${CARGO_REGISTRIES_GROUNDHOG_TOKEN:-}}" ]; then
  export CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_TOKEN:-${CARGO_REGISTRIES_GROUNDHOG_TOKEN}}"
  export CARGO_REGISTRIES_GROUNDHOG_LEGACY_CREDENTIAL_PROVIDER="${CARGO_REGISTRIES_GROUNDHOG_LEGACY_CREDENTIAL_PROVIDER:-cargo:token}"
fi

echo "Installing groundhog-cli..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://crates.thruput.network/api/v1/crates/ \
  cargo "+${TOOL_RUST}" install --locked --registry groundhog groundhog-cli@0.15.1

echo "Installing groundhog-rv32-lower..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://crates.thruput.network/api/v1/crates/ \
  cargo "+${TOOL_RUST}" install --locked --registry groundhog groundhog-rv32-lower@0.15.1
