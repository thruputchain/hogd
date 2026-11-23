#!/usr/bin/env bash
set -Eeuo pipefail

: "${CI_WAIT_PORT_TIMEOUT_SEC:=600}"
. "$(dirname "$0")/bench_harness.sh"
. "$(dirname "$0")/../hog-ip.sh"
ci_enable_error_trace

asan_coordinator_diagnostic=
asan_proposal_timings=
asan_sender_sampler_pid=
asan_sender_enabled=
asan_cc_mode=
coordinator_stats=()
source "$(dirname "$0")/asan_coordinator_stages.sh"
if ci_asan_coordinator_branch; then
  if ! ci_asan_coordinator_selected ||
    [[ ${HOG_DUMP_DIR:-} != /workspace/.ci/dumps || $(git rev-parse HEAD) != "$GIT_COMMIT" ]]; then
    echo 'ASAN coordinator diagnostic requires the exact offline standalone mempool benchmark on hog10' >&2
    exit 1
  fi
  asan_coordinator_diagnostic="$HOG_DUMP_DIR/asan-coordinator-stages"
  case $GIT_BRANCH in
  codex/asan-chuckd-propose-timing) asan_proposal_timings=1 ;;
  codex/asan-chuckd-rpc-breakdown | codex/asan-chuckd-proposal-receive | codex/asan-chuckd-tcp-info | codex/asan-proposal-cc-stock | codex/asan-proposal-cc-reno)
    asan_proposal_timings=1
    coordinator_stats=(--rpc-log-stats)
    ;;
  esac
  case $GIT_BRANCH in
  codex/asan-chuckd-tcp-info) asan_sender_enabled=1 ;;
  codex/asan-proposal-cc-stock | codex/asan-proposal-cc-reno)
    asan_sender_enabled=1
    asan_cc_mode=${GIT_BRANCH##*-}
    ;;
  esac
fi

BENCH_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog11)
STALE_PROCESS_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11)
STALE_PROCESSES="loadgen_server hog-exec hog-event hog-indexer hog-db hog-auth hog-mempool chuckd chuckd.dev scpd scpd.dev"
CONTROL_CLI="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"
WORK_ROOT=$([[ $WORK_DIR = /* ]] && printf '%s' "${WORK_DIR%/}" || printf '%s/%s' "$PWD" "${WORK_DIR%/}")
EVENT_WORK_DIR="$WORK_ROOT/event"
INDEXER_DB_DIR="$WORK_ROOT/indexer"

kill_stale_processes() {
  ci_remote_killall_hosts "$STALE_PROCESSES" "${STALE_PROCESS_HOSTS[@]}"
}

stop_asan_sender_sampler() {
  [[ -n $asan_sender_sampler_pid ]] || return 0
  local status=0 count
  local stop="$asan_coordinator_diagnostic/.stop-sender-tcp-info"
  : >"$stop"
  wait "$asan_sender_sampler_pid" || status=$?
  asan_sender_sampler_pid=
  rm -- "$stop"
  count=$(wc -l <"$asan_coordinator_diagnostic/sender-tcp-info.jsonl")
  printf 'source=%s\nrun=%s\nexit_status=%s\nsamples=%s\n' \
    "$GIT_COMMIT" "$HOG_XID" "$status" "$count" \
    >"$asan_coordinator_diagnostic/sender-tcp-info-status.txt"
}

cleanup() {
  local exit_code=$1

  stop_asan_sender_sampler || :

  ci_enter_cleanup
  ci_control_stop_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"
  kill_stale_processes

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

echo 'set port env vars'
export CI_CONTROL_PORT=$(./$BUILD_DIR/hogd/utils/get_port kCIControlServerPort)

export LOADGEN_PORT=11222
export LOADGEN_HTTP_PORT=11223

export EXECUTOR_MEMPOOL_PORT=8040
export EXECUTOR_HTTP_PORT=8041

export AUTH_DB_PORT=8050
export AUTH_MEMPOOL_PORT=8051
export AUTH_HTTP_PORT=8052

export INDEXER_BLOCK_EXPLORER_PORT=8061
export INDEXER_HTTP_PORT=8062

export MEMPOOL_COORDINATOR_PORT=8070
export MEMPOOL_EXECUTOR_PORT=8071
export MEMPOOL_DB_PORT=8072
export MEMPOOL_EVENT_PORT=8073
export MEMPOOL_AUTH_PORT=8074
export MEMPOOL_HTTP_PORT=8075

export DB_EXECUTOR_PORT=8080
export DB_MEMPOOL_PORT=8081
export DB_AUTH_PORT=8082
export DB_HTTP_PORT=8083

export EVENT_EXECUTOR_PORT=8090
export EVENT_MEMPOOL_PORT=8091
export EVENT_INDEXER_PORT=8092
export EVENT_HTTP_PORT=8093

echo 'make sure bench servers are running'
ci_wait_control_server_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" \
  "${BENCH_HOSTS[@]}"

echo 'clear work dirs'
ci_control_clear_work_dirs "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"

echo 'kill stale processes'
kill_stale_processes

echo 'create event and indexer work dirs'
ci_remote_run hog9 "mkdir -p $(printf '%q' "$EVENT_WORK_DIR") $(printf '%q' "$INDEXER_DB_DIR")"

echo 'launch loadgen'
$CONTROL_CLI hog7 $CI_CONTROL_PORT start hogd/loadgen/loadgen_server --bind-ip "$(hog_ip hog7)" --port $LOADGEN_PORT --metrics-port $LOADGEN_HTTP_PORT

echo 'wait for loadgen'
ci_wait_port hog7 $LOADGEN_PORT

echo 'launch first hog-exec'
# the mempool runs on hog10
$CONTROL_CLI hog5 $CI_CONTROL_PORT start hogd/hog_exec/hog-exec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog5)" --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
# can't wait for hog-exec: that's waiting on the coordinator

echo 'launch second hog-exec'
# the mempool runs on hog10
$CONTROL_CLI hog6 $CI_CONTROL_PORT start hogd/hog_exec/hog-exec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog6)" --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
# can't wait for hog-exec: that's waiting on the coordinator

echo 'check hog-exec is running'
$CONTROL_CLI hog5 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog6 $CI_CONTROL_PORT expect_running

echo 'launch event'
$CONTROL_CLI hog9 $CI_CONTROL_PORT start hogd/hog_event/hog-event --mempool-addr "$(hog_ip hog10)":$MEMPOOL_EVENT_PORT --bind-ip "$(hog_ip hog9)" --work-dir "$EVENT_WORK_DIR" --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT --metrics-port $EVENT_HTTP_PORT

echo 'launch indexer'
$CONTROL_CLI hog9 $CI_CONTROL_PORT start hogd/hog_indexer/hog-indexer --event-addr "$(hog_ip hog9)":$EVENT_INDEXER_PORT --db-path "$INDEXER_DB_DIR" --bind-ip "$(hog_ip hog9)" --block-explorer-port $INDEXER_BLOCK_EXPLORER_PORT --metrics-port $INDEXER_HTTP_PORT

echo 'launch hog-db'
$CONTROL_CLI hog11 $CI_CONTROL_PORT start hogd/hog_db/hog-db --db-path $WORK_DIR/ --ram-bytes 10000000000 --bind-ip "$(hog_ip hog11)" --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_DB_PORT --executor-port $DB_EXECUTOR_PORT --mempool-port $DB_MEMPOOL_PORT --auth-port $DB_AUTH_PORT --metrics-port $DB_HTTP_PORT

echo 'launch auth'
$CONTROL_CLI hog8 $CI_CONTROL_PORT start hogd/hog_auth/hog-auth --mempool-addr "$(hog_ip hog10)":$MEMPOOL_AUTH_PORT --bind-ip "$(hog_ip hog8)" --db-port $AUTH_DB_PORT --mempool-port $AUTH_MEMPOOL_PORT --metrics-port $AUTH_HTTP_PORT

echo 'launch coordinator and scp'
start_coordinator() {
  $CONTROL_CLI hog4 $CI_CONTROL_PORT start \
    chuckd.dev \
    -l info \
    --stack "${coordinator_stats[@]}" \
    --config ./scripts/config-chuckd.json \
    --instance 970c9b \
    --seed 1410441218572558875 \
    --metrics :2223 \
    bench coordinator_loadgen \
    --skip 20 \
    --in-addr "$(hog_ip hog4)":7888 \
    --debug-port 5500 \
    --num-accts 10000 \
    --txs-per-block 1000000 \
    --num-blocks 1000 \
    --network-hash 11111111111111111111111111111112 \
    --storage-dir $WORK_DIR/chuckd-storage \
    --out-addr :50000 \
    --loadgen-addr "$(hog_ip hog7)":$LOADGEN_PORT \
    --sched-addr "$(hog_ip hog10)":$MEMPOOL_COORDINATOR_PORT \
    --scpd-path ./$BUILD_DIR/scpd.dev \
    --scp-handle-addr 127.0.0.1:3300 \
    --scp-control-addr 127.0.0.1:4400
}

if [[ -n $asan_coordinator_diagnostic ]]; then
  mkdir -m 700 "$asan_coordinator_diagnostic"
  if [[ -n $asan_cc_mode ]]; then
    ci_asan_proposal_cc_preflight "$asan_cc_mode" >"$asan_coordinator_diagnostic/tcp-cc-preflight.txt"
  fi
  if [[ -n $asan_proposal_timings ]]; then
    timeout -k 5 240 python3 /opt/hog-ci/peer.py hog4 \
      'bash /workspace/scripts/ci/asan_chuckd_propose.sh' </dev/null >"$asan_coordinator_diagnostic/chuckd-build.txt"
  fi
  coordinator_launch=$(start_coordinator)
  printf '%s\n' "$coordinator_launch"
  coordinator_stdout=$(ci_asan_coordinator_log_path stdout "$coordinator_launch")
  coordinator_stderr=$(ci_asan_coordinator_log_path stderr "$coordinator_launch")
else
  start_coordinator
fi

echo 'make sure procs are running'
$CONTROL_CLI hog4 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog5 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog6 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog7 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog8 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog9 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog11 $CI_CONTROL_PORT expect_running

echo 'launch mempool'
mempool_env=()
if [[ -n $asan_proposal_timings ]]; then
  mempool_env=(env HOGD_ASAN_MEMPOOL_BLOCK_TIMINGS=1)
  if [[ ${#coordinator_stats[@]} != 0 ]]; then
    mempool_env+=(HOGD_ASAN_PROPOSAL_ENCODE_TIMING=1)
  fi
fi
if [[ -n $asan_sender_enabled ]]; then
  mempool_env+=(HOGD_ASAN_SENDER_TCP_INFO=1)
  congestion_args=()
  sampler_receipt=/dev/null
  if [[ -n $asan_cc_mode ]]; then
    mempool_env+=("HOGD_ASAN_PROPOSAL_CC=$asan_cc_mode" "HOGD_ASAN_PROPOSAL_CC_PEER=$(hog_ip hog4)")
    congestion_args=(--congestion-mode "$asan_cc_mode")
    sampler_receipt="$asan_coordinator_diagnostic/tcp-cc-sampler.txt"
  fi
  python3 "$(dirname "$0")/asan_sender_tcp_info.py" \
    --local-ip "$(hog_ip hog10)" --peer-ip "$(hog_ip hog4)" --seconds 185 \
    --stop-file "$asan_coordinator_diagnostic/.stop-sender-tcp-info" "${congestion_args[@]}" \
    >"$asan_coordinator_diagnostic/sender-tcp-info.jsonl" 2>"$sampler_receipt" &
  asan_sender_sampler_pid=$!
fi
set +e
"${mempool_env[@]}" timeout 180s ./$BUILD_DIR/hogd/hog_mempool/hog-mempool \
  --bind-ip "$(hog_ip hog10)" \
  --metrics-port "$MEMPOOL_HTTP_PORT" \
  --network-hash 11111111111111111111111111111112 \
  --num-db-shards 1 \
  --num-exec-shards 2 \
  --db-port "$MEMPOOL_DB_PORT" \
  --event-port "$MEMPOOL_EVENT_PORT" \
  --executor-port "$MEMPOOL_EXECUTOR_PORT" \
  --auth-port "$MEMPOOL_AUTH_PORT" \
  --coordinator-port "$MEMPOOL_COORDINATOR_PORT"
mempool_status=$?
set -e
stop_asan_sender_sampler || :

coordinator_status=0
if ! $CONTROL_CLI hog4 "$CI_CONTROL_PORT" expect_running; then
  set +e
  $CONTROL_CLI hog4 "$CI_CONTROL_PORT" wait
  coordinator_status=$?
  set -e
fi

echo 'copy coordinator output from hog4'
if [[ -n $asan_coordinator_diagnostic ]]; then
  ci_asan_coordinator_collect stdout "$coordinator_stdout" coordinator_output.txt \
    "$asan_coordinator_diagnostic/capture-failure.txt"
  cp -- coordinator_output.txt "$asan_coordinator_diagnostic/blocks.txt"
  full_stdout=$(grep -c ', Num txs in block, 1000000,' coordinator_output.txt)
  printf 'source=%s\nbranch=%s\nrun=%s\nmode=standalone-asan\ncoordinator=hog4\nstdout_full=%s\n' \
    "$GIT_COMMIT" "$GIT_BRANCH" "$HOG_XID" "$full_stdout" >"$asan_coordinator_diagnostic/stdout-provenance.txt"
else
  # We grab the file with the prefix [hogd_stdout] that has the newest timestamp.
  scp hog4:"$(ssh -n hog4 'ls -1t /tmp/hogd_stdout* | head -n 1')" ./coordinator_output.txt
fi

if [[ $mempool_status -ne 0 && $mempool_status -ne 124 ]]; then
  # 124 is [timeout] reaping a mempool that outlived the load, which is the
  # expected end of this benchmark. Anything else is a real failure, and a
  # signal-terminated status is a crash rather than a reported error, so name
  # the signal instead of leaving a bare number in the log.
  echo "mempool $(ci_describe_exit_status "$mempool_status")" >&2
  exit "$mempool_status"
fi
if [[ $coordinator_status -ne 0 ]]; then
  echo "coordinator $(ci_describe_exit_status "$coordinator_status")" >&2
  exit "$coordinator_status"
fi

echo 'coordinator benchmark results'
cat coordinator_output.txt

if [[ -n $asan_coordinator_diagnostic ]]; then
  ci_asan_coordinator_collect stderr "$coordinator_stderr" "$asan_coordinator_diagnostic/info.json" \
    "$asan_coordinator_diagnostic/capture-failure.txt"
  if [[ -n $asan_proposal_timings ]]; then
    python3 -c 'import json,sys; sys.exit(not json.load(sys.stdin).get("matched_committed_proposal_timings"))' \
      <"$asan_coordinator_diagnostic/info.json" || {
      echo 'No proposal caller timing matched a committed one-million-transaction block.' >&2
      exit 1
    }
  fi
  if [[ ${#coordinator_stats[@]} != 0 ]]; then
    python3 "$(dirname "$0")/asan_coordinator_stages.py" check-reply-stats \
      "$(hog_ip hog4)" "$(hog_ip hog10)" "$MEMPOOL_COORDINATOR_PORT" \
      <"$asan_coordinator_diagnostic/info.json"
  fi
  if [[ $GIT_BRANCH == codex/asan-chuckd-proposal-receive || -n $asan_sender_enabled ]]; then
    python3 -c 'import json, sys; sys.exit(not json.load(sys.stdin).get("proposal_receive"))' \
      <"$asan_coordinator_diagnostic/info.json" || {
      echo 'No large scheduler reply receive receipts were collected.' >&2
      exit 1
    }
  fi
  if [[ -n $asan_cc_mode ]]; then
    if ! grep -Fxq exit_status=0 "$asan_coordinator_diagnostic/sender-tcp-info-status.txt"; then
      echo 'ASAN congestion sampler did not confirm a complete single-algorithm socket cohort.' >&2
      exit 1
    fi
  fi
  full_info=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["full_committed_blocks"])' \
    <"$asan_coordinator_diagnostic/info.json")
  printf 'source=%s\nbranch=%s\nrun=%s\nmode=standalone-asan\ncoordinator=hog4\nstdout_full=%s\ninfo_full=%s\n' \
    "$GIT_COMMIT" "$GIT_BRANCH" "$HOG_XID" "$full_stdout" "$full_info" >"$asan_coordinator_diagnostic/provenance.txt"
  rm -- "$asan_coordinator_diagnostic/stdout-provenance.txt"
  exit 0
fi

echo 'clone metrics history # This is idempotent'
./scripts/clone_metrics_history.sh

echo 'log integration metrics'
./scripts/log_metric.sh "${CI_MEMPOOL_METRIC_NAME:-nightly_integration_mempool}" coordinator_output.txt
