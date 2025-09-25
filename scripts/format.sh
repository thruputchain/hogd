#!/bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh

function usage() {
  echo "usage: scripts/format.sh [-c|--check]" >&2
  exit 2
}

# Parse arguments
CHECK="OFF"
for arg in "$@"; do
  shift
  case "${arg}" in
  '--help' | '-h') usage ;;
  '--check' | '-c') CHECK="ON" ;;
  esac
done
if [ "${CHECK}" = "ON" ]; then
  FFLAGS="--dry-run --Werror"
  CF_FLAGS="--check"
  SH_FLAGS="-d"
else
  FFLAGS=""
  CF_FLAGS=""
  SH_FLAGS="-w"
fi

CODE_DIR=${ROOT_DIR}/hogd
X23_DIR=${ROOT_DIR}/x23
SH_DIR=${ROOT_DIR}/scripts

# Make failures exit (for checking)
set -euo pipefail

# Run clang-format
git ls-files -c "${CODE_DIR}/*.cc" "${X23_DIR}/*.cc" \
  "${CODE_DIR}/*.h" "${X23_DIR}/*.h" |
  xargs ${CLANG_FORMAT} -i ${FFLAGS}

# Rust fmt
CARGO_DIRS=$(git ls-files --recurse-submodules -c "${CODE_DIR}/*/Cargo.toml")
for CARGO_DIR in $CARGO_DIRS; do
  cargo fmt ${CF_FLAGS} --manifest-path=${CARGO_DIR} --message-format human
done

# Shell fmt.
SH_FLAGS+=" -i 2"
find "${SH_DIR}" -name '*.sh' |
  xargs ${SH_FORMAT} ${SH_FLAGS}

# xdr format (note: this assumes .clang-format matches that in the protocol directory)
if OFF != "$CHECK"; then
  git-ls-files "${ROOT_DIR}/protocol/*.x" | xargs ${X23_DIR}/misc/xdrformat
fi
