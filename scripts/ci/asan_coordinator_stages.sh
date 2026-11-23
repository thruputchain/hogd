#!/usr/bin/env bash
# Bounded INFO collection over the trusted standalone ASAN run-local broker.

ci_asan_coordinator_selected() {
  [[ ${GIT_BRANCH:-} == codex/asan-mempool-coordinator-stages &&
    ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
    ${HOG_HOST:-} == hog10 && ${HOG_XID:-} =~ ^hogd-mempool-bench-[1-9][0-9]*-[1-9][0-9]*$ &&
    ${CI_MEMPOOL_METRIC_NAME:-} == nightly_integration_mempool_asan &&
    ${GIT_COMMIT:-} =~ ^[0-9a-f]{40}$ ]]
}

ci_asan_coordinator_log_path() {
  local stream=$1 output=$2 key
  local -a paths
  case $stream in
  stdout) key=CONTROL_STDOUT_PATH ;;
  stderr) key=CONTROL_STDERR_PATH ;;
  *) return 1 ;;
  esac
  mapfile -t paths < <(sed -n "s/^$key=//p" <<<"$output")
  [[ ${#paths[@]} == 1 &&
    ${paths[0]} =~ ^/tmp/hogd_${stream}_[0-9]+_[0-9]+_[[:alnum:]]{6}$ ]] || return 1
  printf '%s\n' "${paths[0]}"
}

ci_asan_coordinator_reduce() {
  local stream=$1 path=$2
  [[ $stream == stdout || $stream == stderr ]] || return 1
  [[ $path =~ ^/tmp/hogd_${stream}_[0-9]+_[0-9]+_[[:alnum:]]{6}$ ]] || return 1
  timeout -k 2 20 python3 /opt/hog-ci/peer.py hog4 "
    set -euo pipefail
    file=$path
    [[ -f \$file && ! -L \$file && -O \$file ]]
    [[ \$(realpath -e -- \$file) == \$file ]]
    [[ \$(stat -c %s -- \$file) -le 16777216 ]]
    timeout -k 2 15 python3 /workspace/scripts/ci/asan_coordinator_stages.py $stream < \$file
  " </dev/null
}
