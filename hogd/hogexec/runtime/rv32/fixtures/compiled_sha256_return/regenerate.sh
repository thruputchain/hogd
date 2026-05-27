#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

mode="${1:---check}"
if [[ "${mode}" != "--check" && "${mode}" != "--update" ]]; then
  echo "usage: $0 [--check|--update]" >&2
  exit 2
fi

target="riscv32im-unknown-none-elf"
expected_rustc_release="1.95.0"
profile="release"
elf="target/${target}/${profile}/rv32-compiled-sha256-return"
header="compiled_sha256_return_fixture.h"
metadata="compiled_sha256_return_fixture.metadata"

actual_rustc_release="$(rustc --version --verbose | awk -F': ' '/^release: / {print $2}')"
if [[ "${actual_rustc_release}" != "${expected_rustc_release}" ]]; then
  echo "expected rustc ${expected_rustc_release}, got ${actual_rustc_release}" >&2
  exit 1
fi

if ! rustup target list --installed | grep -qx "${target}"; then
  echo "missing Rust target ${target}; install with: rustup target add ${target}" >&2
  exit 1
fi

export CARGO_TARGET_DIR="${PWD}/target"
export SOURCE_DATE_EPOCH=0

cargo build --release --locked --offline

rustc_version="$(rustc --version --verbose | tr '\n' '; ')"
build_flags="$(printf '%s' 'cargo build --release --locked --offline; Cargo.toml profile.release: panic=abort, opt-level=s, codegen-units=1, lto=false; .cargo/config.toml rustflags: link-arg=-Tlinker.ld, link-arg=--build-id=none, relocation-model=static, target-feature=+m,-a,-c, force-frame-pointers=no')"

write_fixture() {
  local out_header="$1"
  local out_metadata="$2"
  python3 lower_elf.py \
    --elf "${elf}" \
    --header "${out_header}" \
    --metadata "${out_metadata}" \
    --source "src/main.rs" \
    --linker-script "linker.ld" \
    --lowering-tool "lower_elf.py" \
    --rustc-version "${rustc_version}" \
    --target-triple "${target}" \
    --build-flags "${build_flags}" \
    --metadata-input "Cargo.toml" \
    --metadata-input "Cargo.lock" \
    --metadata-input ".cargo/config.toml" \
    --metadata-input "rust-toolchain.toml"
}

tmp_header_1="$(mktemp)"
tmp_metadata_1="$(mktemp)"
tmp_header_2="$(mktemp)"
tmp_metadata_2="$(mktemp)"
trap 'rm -f "${tmp_header_1}" "${tmp_metadata_1}" "${tmp_header_2}" "${tmp_metadata_2}"' EXIT

write_fixture "${tmp_header_1}" "${tmp_metadata_1}"

cargo build --release --locked --offline
write_fixture "${tmp_header_2}" "${tmp_metadata_2}"

cmp "${tmp_header_1}" "${tmp_header_2}"
cmp "${tmp_metadata_1}" "${tmp_metadata_2}"

if [[ "${mode}" == "--update" ]]; then
  cp "${tmp_header_1}" "${header}"
  cp "${tmp_metadata_1}" "${metadata}"
else
  cmp "${tmp_header_1}" "${header}"
  cmp "${tmp_metadata_1}" "${metadata}"
fi
