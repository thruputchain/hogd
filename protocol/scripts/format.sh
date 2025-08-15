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
else
    FFLAGS=""
fi

CODE_DIR=${ROOT_DIR}

# Make failures exit (for checking)
set -euo pipefail

# xdr format
find "${CODE_DIR}" -name '*.x' |\
xargs ${CLANG_FORMAT} -i ${FFLAGS}
