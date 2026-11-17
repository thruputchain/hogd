#!/bin/sh
# Generate Rust from an XDR file with thruput-xdr-codegen
# Usage: gen_xdr_rs.sh <output.rs> <input.x>
#
# thruput-xdr-codegen is built from the commit of thruputchain/xdrs pinned in
# scripts/xdr-codegen-rev; scripts/fetch.sh installs it.

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

if [ "$#" -ne 2 ]; then
    echo "Usage: gen_xdr_rs.sh <output.rs> <input.x>" >&2
    exit 1
fi

# Version gate
XDR_CODEGEN_REV="$(cat "${SCRIPT_DIR}/../scripts/xdr-codegen-rev")"
ACTUAL_VERSION="$(thruput-xdr-codegen --version 2>/dev/null || true)"
case "${ACTUAL_VERSION}" in
    *"(xdrs ${XDR_CODEGEN_REV})") ;;
    *)
        echo "error: expected thruput-xdr-codegen built from xdrs ${XDR_CODEGEN_REV}, got '${ACTUAL_VERSION:-not installed}'. Run scripts/fetch.sh" >&2
        exit 1
        ;;
esac

exec thruput-xdr-codegen --rust-no-feature serde --rust-no-feature schema \
    --rust-feature xdrs --rust-no-feature spec -o "$1" "$2"
