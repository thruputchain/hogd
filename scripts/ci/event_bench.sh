#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
. "$(dirname "$0")/../hog-ip.sh"

CONTROL_CLI="./$BUILD_DIR/hogd/hogexec/bench/control_cli"
EVENT_BENCH_HOST=${EVENT_BENCH_HOST:-hog11}
EVENT_BENCH_CONTROL_PORT=${EVENT_BENCH_CONTROL_PORT:-${CI_CONTROL_PORT:-11111}}
EVENT_BENCH_FAKE_SCHED_HOST=${EVENT_BENCH_FAKE_SCHED_HOST:-${EVENT_BENCH_MEMPOOL_HOST:-$(hostname)}}
EVENT_BENCH_FAKE_SCHED_PORT=${EVENT_BENCH_FAKE_SCHED_PORT:-${EVENT_BENCH_MEMPOOL_PORT:-8095}}
EVENT_BENCH_WORK_DIR=${EVENT_BENCH_WORK_DIR:-${WORK_DIR:-ci_work_dir}}
EVENT_BENCH_FAKE_SCHED_PID=""
EVENT_BENCH_EVENT_STDOUT=""
EVENT_BENCH_EVENT_STDERR=""
EVENT_BENCH_REMOTE_WORK_DIR=""

dump_local_log() {
  local label=$1
  local path=$2

  if [ ! -f "$path" ]; then
    return 0
  fi
  echo "--- $label: $path ---"
  tail -n 200 "$path" || true
}

dump_event_exec_logs() {
  local path

  for path in "$EVENT_BENCH_EVENT_STDOUT" "$EVENT_BENCH_EVENT_STDERR"; do
    if [ -z "$path" ]; then
      continue
    fi
    echo "--- event_exec log: $EVENT_BENCH_HOST:$path ---"
    ci_remote_run "$EVENT_BENCH_HOST" "if [ -f '$path' ]; then tail -n 200 '$path'; else echo 'missing $path'; fi" || true
  done
}

resolve_event_work_dir() {
  local control_work_dir
  local output
  local suffix
  local work_prefix

  if [[ $EVENT_BENCH_WORK_DIR = /* ]]; then
    EVENT_BENCH_REMOTE_WORK_DIR=$EVENT_BENCH_WORK_DIR
    return 0
  fi

  if ! output=$("$CONTROL_CLI" "$EVENT_BENCH_HOST" "$EVENT_BENCH_CONTROL_PORT" work_dir 2>&1); then
    printf '%s\n' "$output"
    return 1
  fi
  printf '%s\n' "$output"
  control_work_dir=$(printf '%s\n' "$output" | sed -n 's/^CONTROL_WORK_DIR=//p' | tail -n 1)
  if [[ -z $control_work_dir ]]; then
    echo 'failed to parse control server work directory' >&2
    return 1
  fi

  work_prefix=${WORK_DIR:-ci_work_dir}
  work_prefix=${work_prefix%/}
  if [[ -n $work_prefix &&
        ($EVENT_BENCH_WORK_DIR == "$work_prefix" ||
         $EVENT_BENCH_WORK_DIR == "$work_prefix/"*) ]]; then
    suffix=${EVENT_BENCH_WORK_DIR#"$work_prefix"}
    suffix=${suffix#/}
    EVENT_BENCH_REMOTE_WORK_DIR=$control_work_dir
    if [[ -n $suffix ]]; then
      EVENT_BENCH_REMOTE_WORK_DIR="$EVENT_BENCH_REMOTE_WORK_DIR/$suffix"
    fi
  else
    EVENT_BENCH_REMOTE_WORK_DIR="$control_work_dir/$EVENT_BENCH_WORK_DIR"
  fi
}

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  if [ "$exit_code" -ne 0 ]; then
    dump_event_exec_logs
    dump_local_log 'event fake scheduler log' event_fake_sched_log
    dump_local_log 'event server benchmark log' event_server_bench_log
    dump_local_log 'event server benchmark stderr' event_server_bench_err
  fi
  if [ -n "$EVENT_BENCH_FAKE_SCHED_PID" ]; then
    echo 'stop fake scheduler event server'
    kill "$EVENT_BENCH_FAKE_SCHED_PID" 2>/dev/null || true
    wait "$EVENT_BENCH_FAKE_SCHED_PID" 2>/dev/null || true
  fi
  echo 'stop event server'
  "$CONTROL_CLI" "$EVENT_BENCH_HOST" "$EVENT_BENCH_CONTROL_PORT" stop || true

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

echo 'set port env vars'
export EVENT_EXECUTOR_PORT=${EVENT_EXECUTOR_PORT:-8090}
export EVENT_MEMPOOL_PORT=${EVENT_MEMPOOL_PORT:-8091}
export EVENT_INDEXER_PORT=${EVENT_INDEXER_PORT:-8092}

echo 'make sure bench server is running'
ci_wait_port "$EVENT_BENCH_HOST" "$EVENT_BENCH_CONTROL_PORT"

echo 'clear work dirs'
"$CONTROL_CLI" "$EVENT_BENCH_HOST" "$EVENT_BENCH_CONTROL_PORT" clear_work_dir
resolve_event_work_dir
echo "create event work dir: $EVENT_BENCH_REMOTE_WORK_DIR"
ci_remote_run "$EVENT_BENCH_HOST" "mkdir -p '$EVENT_BENCH_REMOTE_WORK_DIR'"

echo 'start fake scheduler event server'
./$BUILD_DIR/hogd/event/event_server_bench -F -p "$EVENT_BENCH_FAKE_SCHED_PORT" >event_fake_sched_log 2>&1 &
EVENT_BENCH_FAKE_SCHED_PID=$!
ci_wait_port "$EVENT_BENCH_FAKE_SCHED_HOST" "$EVENT_BENCH_FAKE_SCHED_PORT"

echo 'launch event server'
if ! event_start_output=$("$CONTROL_CLI" "$EVENT_BENCH_HOST" "$EVENT_BENCH_CONTROL_PORT" start hogd/event/event_exec --mempool-addr "$(hog_ip "$EVENT_BENCH_FAKE_SCHED_HOST"):$EVENT_BENCH_FAKE_SCHED_PORT" --bind-ip "$(hog_ip "$EVENT_BENCH_HOST")" --work-dir "$EVENT_BENCH_REMOTE_WORK_DIR/" --executor-port "$EVENT_EXECUTOR_PORT" --mempool-port "$EVENT_MEMPOOL_PORT" --indexer-port "$EVENT_INDEXER_PORT" 2>&1); then
  printf '%s\n' "$event_start_output"
  exit 1
fi
printf '%s\n' "$event_start_output"
EVENT_BENCH_EVENT_STDOUT=$(printf '%s\n' "$event_start_output" | sed -n 's/^CONTROL_STDOUT_PATH=//p' | tail -n 1)
EVENT_BENCH_EVENT_STDERR=$(printf '%s\n' "$event_start_output" | sed -n 's/^CONTROL_STDERR_PATH=//p' | tail -n 1)
if [[ -z $EVENT_BENCH_EVENT_STDOUT || -z $EVENT_BENCH_EVENT_STDERR ]]; then
  echo 'failed to parse event_exec control log paths' >&2
  exit 1
fi

echo 'make sure event server is running'
"$CONTROL_CLI" "$EVENT_BENCH_HOST" "$EVENT_BENCH_CONTROL_PORT" expect_running

echo 'wait for event server ports'
ci_wait_port "$EVENT_BENCH_HOST" "$EVENT_EXECUTOR_PORT"
ci_wait_port "$EVENT_BENCH_HOST" "$EVENT_MEMPOOL_PORT"

echo 'run event server benchmark'
./$BUILD_DIR/hogd/event/event_server_bench -S -e "$(hog_ip "$EVENT_BENCH_HOST"):$EVENT_EXECUTOR_PORT" -s "$(hog_ip "$EVENT_BENCH_HOST"):$EVENT_MEMPOOL_PORT" -n 1000000 -c 40 -o 100 -p "$EVENT_BENCH_FAKE_SCHED_PORT" >event_server_bench_log 2>event_server_bench_err

if [ "${CI_SKIP_CLONE_METRICS_HISTORY:-0}" != "1" ]; then
  echo 'clone metrics history' # This is idempotent
  ./scripts/clone_metrics_history.sh
fi

echo 'log event server metrics'
./scripts/log_metric.sh event_server_bench event_server_bench_log
