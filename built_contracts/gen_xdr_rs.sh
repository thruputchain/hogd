#!/bin/sh
# Generate Rust from an XDR file with xdrc
# Usage: gen_xdr_rs.sh <output.rs> <input.x>
#
# xdrc is built from the commit of thruputchain/xdrs pinned in
# scripts/xdrc-rev; scripts/fetch.sh installs it.

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

if [ "$#" -ne 2 ]; then
    echo "Usage: gen_xdr_rs.sh <output.rs> <input.x>" >&2
    exit 1
fi

# Version gate
XDRC_REV="$(cat "${SCRIPT_DIR}/../scripts/xdrc-rev")"
ACTUAL_VERSION="$(xdrc --version 2>/dev/null || true)"
case "${ACTUAL_VERSION}" in
    *"(xdrs ${XDRC_REV})") ;;
    *)
        echo "error: expected xdrc built from xdrs ${XDRC_REV}, got '${ACTUAL_VERSION:-not installed}'. Run scripts/fetch.sh" >&2
        exit 1
        ;;
esac

exec xdrc --rust-no-feature serde --rust-no-feature schema \
    --rust-feature xdrs --rust-no-feature spec -o "$1" "$2"
