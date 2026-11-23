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
  local stream=$1 path=$2 limit
  case $stream in
  stdout) limit=1048576 ;;
  stderr) limit=268435456 ;;
  *)
    echo 'Invalid ASAN coordinator log stream.' >&2
    return 1
    ;;
  esac
  if [[ ! $path =~ ^/tmp/hogd_${stream}_[0-9]+_[0-9]+_[[:alnum:]]{6}$ ]]; then
    echo "Invalid typed ASAN coordinator $stream log path." >&2
    return 1
  fi
  local remote command
  remote=$(
    cat <<'REMOTE'
set -euo pipefail
stream=$1 file=$2 limit=$3
fail_capture() { printf 'hog4 coordinator %s capture: %s\n' "$stream" "$1" >&2; exit 1; }
[[ ! -L $file ]] || fail_capture 'typed log is a symlink'
[[ -e $file ]] || fail_capture 'typed log is missing'
[[ -f $file ]] || fail_capture 'typed log is not regular'
[[ -O $file ]] || fail_capture 'typed log is not owned by the run user'
[[ -r $file ]] || fail_capture 'typed log is not readable'
resolved=$(realpath -e -- "$file") || fail_capture 'cannot resolve typed log'
[[ $resolved == "$file" ]] || fail_capture 'typed log resolved to a different path'
bytes=$(stat -c %s -- "$file") || fail_capture 'cannot stat typed log'
[[ $bytes =~ ^[0-9]{1,18}$ ]] || fail_capture 'invalid typed log byte size'
((bytes > 0)) || fail_capture 'typed log is empty'
((bytes <= limit)) || fail_capture "typed log has $bytes bytes; limit is $limit"
exec 3<"$file" || fail_capture 'cannot open typed log'
timeout -k 2 30 python3 /workspace/scripts/ci/asan_coordinator_stages.py "$stream" "$bytes" <&3 ||
  fail_capture "bounded parser failed (exit $?)"
REMOTE
  )
  printf -v command 'set -- %q %q %q\n%s' "$stream" "$path" "$limit" "$remote"
  timeout -k 2 40 python3 /opt/hog-ci/peer.py hog4 "$command" </dev/null
}

ci_asan_coordinator_collect() {
  local stream=$1 path=$2 destination=$3 failure=$4 errors status
  errors=$(mktemp)
  if ci_asan_coordinator_reduce "$stream" "$path" >"$destination.pending" 2>"$errors"; then
    rm -f -- "$errors"
    mv -- "$destination.pending" "$destination"
    return
  else
    status=$?
  fi
  {
    printf 'stream=%s\nstatus=%s\n' "$stream" "$status"
    head -c 4096 -- "$errors"
  } >"$failure"
  cat -- "$failure" >&2
  rm -f -- "$errors" "$destination.pending"
  return "$status"
}
