#!/usr/bin/env bash
# Shared defaults for CMake, local Cargo commands, and CI. Safe to source.

hogd_sccache_env() {
  local variable configured=0 addresses
  # An explicit backend/configuration always wins over cluster discovery.
  for variable in SCCACHE_CONF SCCACHE_WEBDAV_ENDPOINT SCCACHE_DIR \
    SCCACHE_BUCKET SCCACHE_REDIS SCCACHE_MEMCACHED SCCACHE_GCS_BUCKET \
    SCCACHE_AZURE_CONNECTION_STRING SCCACHE_GHA_ENABLED SCCACHE_OSS_BUCKET SCCACHE_COS_BUCKET; do
    if printenv "$variable" >/dev/null; then
      configured=1
      break
    fi
  done
  if [[ "$configured" == 0 && "${HOGD_SCCACHE_REMOTE:-1}" != 0 ]] && command -v ip >/dev/null 2>&1; then
    addresses="$(ip -o -4 addr show 2>/dev/null)" || addresses=""
    if [[ "$addresses" =~ [[:space:]]inet[[:space:]]10\.100\. ]]; then
      export SCCACHE_WEBDAV_ENDPOINT=http://hog12:9281/
    fi
  fi
  if [[ -n "${SCCACHE_WEBDAV_ENDPOINT:-}" ]]; then
    export SCCACHE_WEBDAV_KEY_PREFIX="${SCCACHE_WEBDAV_KEY_PREFIX-hogd/rust/}"
  fi
  export CARGO_INCREMENTAL=0
}

hogd_sccache_env
unset -f hogd_sccache_env

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  if [[ "${1:-}" != --print-env || $# != 1 ]]; then
    echo "usage: source scripts/sccache-env.sh OR scripts/sccache-env.sh --print-env" >&2
    exit 2
  fi
  # Persist non-secret settings into generated Ninja and CTest commands. Auth
  # credentials remain in the caller's environment or private sccache config.
  for variable in SCCACHE_WEBDAV_ENDPOINT SCCACHE_WEBDAV_KEY_PREFIX \
    SCCACHE_MULTILEVEL_CHAIN SCCACHE_DIR SCCACHE_CONF SCCACHE_BASEDIRS \
    SCCACHE_SERVER_UDS SCCACHE_SERVER_PORT; do
    if printenv "$variable" >/dev/null; then
      printf '%s=%s\n' "$variable" "${!variable}"
    fi
  done
fi
