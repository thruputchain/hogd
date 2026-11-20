#!/bin/bash

set -euo pipefail

SCRIPT_PATH="${BASH_SOURCE[0]}"
if [[ "${SCRIPT_PATH}" != */* ]]; then
  SCRIPT_PATH="$(command -v "${SCRIPT_PATH}")"
fi

SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd -P)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd -P)"

cd "${ROOT_DIR}"

PROTOCOL_HASH=$(git -C protocol rev-parse HEAD)
GO_FILE="thruput-go/protocol-go/hogp.go"
GO_HASH=$(sed -n 's/^const ProtocolGitCommit string = "\([0-9a-f]\{40\}\)"/\1/p' "${GO_FILE}")

if [ -z "${GO_HASH}" ]; then
  printf "error: failed to parse ProtocolGitCommit from %s\n" "${GO_FILE}" >&2
  exit 1
fi

if ! [[ "${PROTOCOL_HASH}" =~ ^[0-9a-f]{40}$ ]]; then
  printf "error: protocol HEAD is not a 40-char lowercase git hash: %s\n" "${PROTOCOL_HASH}" >&2
  exit 1
fi

if ! [[ "${GO_HASH}" =~ ^[0-9a-f]{40}$ ]]; then
  printf "error: ProtocolGitCommit is not a 40-char lowercase git hash: %s\n" "${GO_HASH}" >&2
  exit 1
fi

if [ "${PROTOCOL_HASH}" != "${GO_HASH}" ]; then
  printf "protocol hash mismatch\n" >&2
  printf "protocol/ HEAD: %s\n" "${PROTOCOL_HASH}" >&2
  printf "thruput-go/protocol-go/ hash: %s\n" "${GO_HASH}" >&2
  exit 1
fi

printf "protocol hash check passed: %s\n" "${PROTOCOL_HASH}"
