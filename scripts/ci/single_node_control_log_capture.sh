#!/usr/bin/env bash
# Private confirmation: record the exact launch receipt, then archive bounded
# role logs only after the timed stage has stopped its writers.
declare -a SINGLE_NODE_CONTROL_LOGS=()

single_node_start_role() {
  local host=$1 label=$2
  shift 2
  if [[ -z $split_storage ]]; then
    "$CONTROL_CLI" "$host" "$CI_CONTROL_PORT" start "$@"
    return
  fi
  local output stdout_path stderr_path
  if ! output=$("$CONTROL_CLI" "$host" "$CI_CONTROL_PORT" start "$@" 2>&1); then
    printf '%s\n' "$output"
    return 1
  fi
  printf '%s\n' "$output"
  stdout_path=$(sed -n 's/^CONTROL_STDOUT_PATH=//p' <<<"$output")
  stderr_path=$(sed -n 's/^CONTROL_STDERR_PATH=//p' <<<"$output")
  if [[ ! $stdout_path =~ ^/tmp/hogd_stdout_[A-Za-z0-9_]+$ ||
        ! $stderr_path =~ ^/tmp/hogd_stderr_[A-Za-z0-9_]+$ ]]; then
    echo "missing or invalid control log receipt for $label on $host" >&2
    return 1
  fi
  SINGLE_NODE_CONTROL_LOGS+=("$host|$label|$stdout_path|$stderr_path")
}

single_node_collect_role_logs() {
  local dest="${HOG_DUMP_DIR:?}/single-node-role-logs"
  local record host role stdout_path stderr_path kind source target
  local status=0 CI_LOG_TAIL_BYTES=1048576
  mkdir -p -- "$dest" || return 1
  printf 'host\trole\tstdout_path\tstderr_path\n' > "$dest/launch-paths.tsv" || return 1
  for record in "${SINGLE_NODE_CONTROL_LOGS[@]}"; do
    IFS='|' read -r host role stdout_path stderr_path <<<"$record"
    printf '%s\t%s\t%s\t%s\n' "$host" "$role" "$stdout_path" "$stderr_path" >> "$dest/launch-paths.tsv" || return 1
    for kind in stdout stderr; do
      source=$stdout_path
      [[ $kind == stdout ]] || source=$stderr_path
      target="$dest/${host}_${role}.${kind}.log"
      if ! scp -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no "$host:$source" "$target" ||
         ! ci_truncate_log_to_tail "$target"; then
        echo "failed to retain bounded $host $role $kind log" >&2
        status=1
      fi
    done
  done
  return "$status"
}
