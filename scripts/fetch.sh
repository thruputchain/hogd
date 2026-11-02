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
# --force: ~/.cargo may hold `thruput` from the old `thruput-cli` package,
# which cargo will not overwrite otherwise. Skip the install when the CLI is
# already current, so --force does not rebuild it on every run.
THRUPUT_CLI_VERSION=0.42.0
THRUPUT_BIN="${CARGO_HOME:-$HOME/.cargo}/bin/thruput"
if [ "$("$THRUPUT_BIN" --version 2>/dev/null | awk '{print $2}')" != "${THRUPUT_CLI_VERSION}" ]; then
  echo "Installing the thruput CLI..."
  cargo "+${TOOL_RUST}" install --locked --force "thruput@${THRUPUT_CLI_VERSION}"
fi

# Hand-written VM fixtures use the low-level, spec-less artifact writer.
RV32_LOWER_BIN="${CARGO_HOME:-$HOME/.cargo}/bin/rv32-lower"
if [ "$("$RV32_LOWER_BIN" --version 2>/dev/null | awk '{print $2}')" != "${THRUPUT_CLI_VERSION}" ]; then
  echo "Installing the RV32 lowerer..."
  cargo "+${TOOL_RUST}" install --locked --force "thruput-rv32-lower@${THRUPUT_CLI_VERSION}"
fi
