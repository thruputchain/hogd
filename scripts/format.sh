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
source ${SCRIPT_DIR}/env.sh 2>/dev/null
ROOT_DIR=${SCRIPT_DIR}/..
CODE_DIR=${ROOT_DIR}/hogd
if [ -z ${CLANG_FORMAT+x} ]; then CLANG_FORMAT=clang-format-18; fi

# Run clang-format
find "${CODE_DIR}" -name '*.h' -o -name '*.cc' |\
xargs ${CLANG_FORMAT} -i ${FFLAGS}
