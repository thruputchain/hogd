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

CODE_DIR=${ROOT_DIR}/hogd

# Run clang-format
find "${CODE_DIR}" -name '*.h' -o -name '*.cc' |\
xargs ${CLANG_FORMAT} -i ${FFLAGS}

set -e
# Rust fmt
CARGO_DIRS=$(find "${CODE_DIR}" -name 'Cargo.toml')
for CARGO_DIR in $CARGO_DIRS; do
    cargo fmt ${CF_FLAGS} --manifest-path=${CARGO_DIR} --message-format human
done

# xdr format
find "${CODE_DIR}" -name '*.x' |\
xargs ${CLANG_FORMAT} -i ${FFLAGS}
