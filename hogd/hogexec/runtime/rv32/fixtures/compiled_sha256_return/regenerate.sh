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
expected_lowerer_version="rv32-lower 0.5.7"
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

rv32_lower_bin="${RV32_LOWER_BIN:-$(command -v rv32-lower || true)}"
if [[ -z "${rv32_lower_bin}" ]]; then
  echo "rv32-lower not found; set RV32_LOWER_BIN or put rv32-lower on PATH" >&2
  exit 1
fi
if [[ ! -x "${rv32_lower_bin}" ]]; then
  echo "rv32-lower is not executable: ${rv32_lower_bin}" >&2
  exit 1
fi
actual_lowerer_version="$(${rv32_lower_bin} --version)"
if [[ "${actual_lowerer_version}" != "${expected_lowerer_version}" ]]; then
  echo "expected ${expected_lowerer_version}, got ${actual_lowerer_version}" >&2
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
  local out_artifact="$3"
  local out_lowerer_metadata="$4"
  "${rv32_lower_bin}" \
    --elf "${elf}" \
    --out "${out_artifact}" \
    --metadata "${out_lowerer_metadata}" \
    --rustc-version "${rustc_version}" \
    --target-triple "${target}" \
    --build-flags "${build_flags}"
  python3 package_artifact.py \
    --elf "${elf}" \
    --artifact "${out_artifact}" \
    --lowerer-metadata "${out_lowerer_metadata}" \
    --header "${out_header}" \
    --metadata "${out_metadata}" \
    --source "src/main.rs" \
    --linker-script "linker.ld" \
    --packaging-tool "package_artifact.py" \
    --lowering-tool "rv32-lower" \
    --lowering-tool-version "${actual_lowerer_version}" \
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
tmp_artifact_1="$(mktemp)"
tmp_lowerer_metadata_1="$(mktemp)"
tmp_header_2="$(mktemp)"
tmp_metadata_2="$(mktemp)"
tmp_artifact_2="$(mktemp)"
tmp_lowerer_metadata_2="$(mktemp)"
trap 'rm -f "${tmp_header_1}" "${tmp_metadata_1}" "${tmp_artifact_1}" "${tmp_lowerer_metadata_1}" "${tmp_header_2}" "${tmp_metadata_2}" "${tmp_artifact_2}" "${tmp_lowerer_metadata_2}"' EXIT

write_fixture "${tmp_header_1}" "${tmp_metadata_1}" "${tmp_artifact_1}" "${tmp_lowerer_metadata_1}"

cargo build --release --locked --offline
write_fixture "${tmp_header_2}" "${tmp_metadata_2}" "${tmp_artifact_2}" "${tmp_lowerer_metadata_2}"

cmp "${tmp_artifact_1}" "${tmp_artifact_2}"
cmp "${tmp_lowerer_metadata_1}" "${tmp_lowerer_metadata_2}"
cmp "${tmp_header_1}" "${tmp_header_2}"
cmp "${tmp_metadata_1}" "${tmp_metadata_2}"

if [[ "${mode}" == "--update" ]]; then
  cp "${tmp_header_1}" "${header}"
  cp "${tmp_metadata_1}" "${metadata}"
else
  cmp "${tmp_header_1}" "${header}"
  cmp "${tmp_metadata_1}" "${metadata}"
fi
