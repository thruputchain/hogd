#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POST_CHECKOUT=${SCRIPT_DIR}/../.git/hooks/post-checkout
POST_MERGE=${SCRIPT_DIR}/../.git/hooks/post-merge

cp "${SCRIPT_DIR}/script-for-hooks.sh" "${POST_CHECKOUT}"
chmod +x "${POST_CHECKOUT}"

cp "${SCRIPT_DIR}/script-for-hooks.sh" "${POST_MERGE}"
chmod +x "${POST_MERGE}"
