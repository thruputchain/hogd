#!/bin/bash

function usage() {
    echo "usage: scripts/format.sh [-c|--check]" >&2
    exit 2
}

# Parse arguments
CHECK="OFF"
for arg in "$@"; do
    shift
    case "${arg}" in
        '--help'|'-h') usage ;;
        '--check'|'-c') CHECK="ON";;
    esac
done
if [ "${CHECK}" = "ON" ];
then
    FFLAGS="--dry-run --Werror"
    CF_FLAGS="--check"
else
    FFLAGS=""
    CF_FLAGS=""
fi

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh

ROOT_DIR=${SCRIPT_DIR}/..
CODE_DIR=${ROOT_DIR}/hogd
if [ -z ${CLANG_FORMAT+x} ]; then CLANG_FORMAT=clang-format-18; fi

# Run clang-format
find "${CODE_DIR}" -name '*.h' -o -name '*.cc' |\
xargs ${CLANG_FORMAT} -i ${FFLAGS}

# Rust fmt
CARGO_DIRS=$(find "${CODE_DIR}" -name 'Cargo.toml')
for CARGO_DIR in $CARGO_DIRS; do
    cargo fmt ${CF_FLAGS} --manifest-path=${CARGO_DIR} --message-format human
done

