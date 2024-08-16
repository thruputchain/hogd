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
else
    FFLAGS=""
fi

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/..
CODE_DIR=${ROOT_DIR}/hogd
CLANG_FORMAT=clang-format-18

# Run clang-format
find "${CODE_DIR}" -name '*.h' -o -name '*.cc' |\
xargs ${CLANG_FORMAT} -i ${FFLAGS}
