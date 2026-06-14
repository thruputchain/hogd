#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cargo_in() {
  local dir="$1"
  shift

  if command -v rustup >/dev/null 2>&1 && [ -f "${dir}/rust-toolchain.toml" ]; then
    local toolchain
    toolchain="$(
      sed -n 's/^[[:space:]]*channel[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
        "${dir}/rust-toolchain.toml" | { head -n 1; cat > /dev/null; }
    )"
    if [ -n "${toolchain}" ]; then
      local rustc
      rustc="$(rustup which --toolchain "${toolchain}" rustc)"
      (cd "${dir}" && RUSTC="${rustc}" rustup run "${toolchain}" cargo "$@")
      return
    fi
  fi

  (cd "${dir}" && cargo "$@")
}

cargo_in "${ROOT_DIR}/built_contracts" fetch -Zbuild-std
cargo_in "${ROOT_DIR}/built_contracts" fetch -Zbuild-std --target wasm32-unknown-unknown
cargo_in "${ROOT_DIR}/built_contracts" fetch -Zbuild-std --target riscv32im-unknown-none-elf
cargo_in "${ROOT_DIR}/hogd/rust" fetch

set -x

echo "Installing groundhog-cli..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://groundhog.dev/crates/api/v1/crates/ \
  cargo install --registry groundhog groundhog-cli@0.5.7

echo "Installing groundhog-rv32-lower..."
CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://groundhog.dev/crates/api/v1/crates/ \
  cargo install --registry groundhog groundhog-rv32-lower@0.5.7
