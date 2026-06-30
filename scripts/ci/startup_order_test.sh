#!/usr/bin/env bash
# Launch the full set of node binaries on localhost in a deliberately-varied
# order and assert that no process crashes on startup. No hog_sched, no hogx.
#
# Usage: startup_order_test.sh <scenario>
#   scenarios: mempool-first mempool-middle mempool-last
#              indexer-before-event event-before-indexer
#              coordinator-first
#
# Env overrides (defaults target the namespace cpp gcc-debug build layout):
#   BUILD_DIR   build directory holding the C++ binaries (default build/gcc)
#   CHUCKD_BIN  path to chuckd.dev   (default $BUILD_DIR/chuckd.dev)
#   SCPD_BIN    path to scpd.dev     (default $BUILD_DIR/scpd.dev)
#   SETTLE_SECS seconds to wait after the last launch (default 25)
#   STAGGER_SECS seconds between launches (default 3, for predictable ordering)
#   HOG_STARTUP_LIST_ONLY=1  print the resolved launch order and exit (no launch)
set -Eeuo pipefail

usage() {
  echo "Usage: $0 <scenario>" >&2
  echo "  scenarios: mempool-first mempool-middle mempool-last indexer-before-event event-before-indexer coordinator-first" >&2
}

SCENARIO="${1:-}"
if [[ -z "$SCENARIO" ]]; then
  usage
  exit 2
fi

# Resolve the launch order (space-separated component keys) for the scenario.
case "$SCENARIO" in
mempool-first)
  ORDER=(mempool loadgen hogexec event indexer hogdb auth chuckd)
  ;;
mempool-last)
  ORDER=(loadgen hogexec event indexer hogdb auth mempool chuckd)
  ;;
mempool-middle)
  ORDER=(loadgen hogexec event mempool indexer hogdb auth chuckd)
  ;;
indexer-before-event)
  ORDER=(mempool loadgen hogexec indexer event hogdb auth chuckd)
  ;;
event-before-indexer)
  ORDER=(mempool loadgen hogexec event indexer hogdb auth chuckd)
  ;;
coordinator-first)
  # Reproduction attempt: bring up the mempool hub + loadgen + coordinator
  # early, but defer the mempool's downstream services (hogexec, hogdb, auth)
  # to the end. The coordinator can then start driving transactions into the
  # mempool before auth/executor/db have connected, so the mempool's
  # schedulercoord_submit_unvetted path forwards an RPC onto a not-yet-ready
  # fd. Connection-level retry covers a peer that is merely *dialing in* late;
  # it does NOT cover an RPC *issued* on a downstream that has not registered.
  # If that is the order-dependent crash, this scenario fails (mempool aborts
  # with "RPC call on failed file descriptor"). auth is launched last to widen
  # the window.
  ORDER=(mempool loadgen chuckd event indexer hogexec hogdb auth)
  ;;
*)
  echo "unknown scenario: $SCENARIO" >&2
  usage
  exit 2
  ;;
esac

if [[ "${HOG_STARTUP_LIST_ONLY:-}" == "1" ]]; then
  printf '%s\n' "${ORDER[@]}"
  exit 0
fi

BUILD_DIR="${BUILD_DIR:-build/gcc}"
CHUCKD_BIN="${CHUCKD_BIN:-$BUILD_DIR/chuckd.dev}"
SCPD_BIN="${SCPD_BIN:-$BUILD_DIR/scpd.dev}"
SETTLE_SECS="${SETTLE_SECS:-25}"
# Stagger launches by a few seconds so a service genuinely starts before the
# next one — the launch order is then the actual wall-clock order, which is
# what these scenarios are predicated on (e.g. coordinator-first needs the
# coordinator to truly precede auth/hogexec/hogdb).
STAGGER_SECS="${STAGGER_SECS:-3}"
IP=127.0.0.1

# Repo root (this script lives in scripts/ci/).
REPO_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
cd "$REPO_ROOT"

# Ports (verbatim from scripts/launch-multi.sh).
LOADGEN_PORT=11222
LOADGEN_HTTP_PORT=11223
EXECUTOR_MEMPOOL_PORT=8040
EXECUTOR_HTTP_PORT=8041
AUTH_DB_PORT=8050
AUTH_MEMPOOL_PORT=8051
AUTH_HTTP_PORT=8052
INDEXER_EVENT_PORT=8060
INDEXER_BLOCK_EXPLORER_PORT=8061
INDEXER_HTTP_PORT=8062
MEMPOOL_COORDINATOR_PORT=8070
MEMPOOL_EXECUTOR_PORT=8071
MEMPOOL_DB_PORT=8072
MEMPOOL_EVENT_PORT=8073
MEMPOOL_AUTH_PORT=8074
MEMPOOL_HTTP_PORT=8075
DB_EXECUTOR_PORT=8080
DB_MEMPOOL_PORT=8081
DB_AUTH_PORT=8082
DB_HTTP_PORT=8083
EVENT_EXECUTOR_PORT=8090
EVENT_MEMPOOL_PORT=8091
EVENT_INDEXER_PORT=8092
EVENT_HTTP_PORT=8093

# Binary paths.
LOADGEN_BIN="$BUILD_DIR/hogd/loadgen/loadgen_server"
MEMPOOL_BIN="$BUILD_DIR/hogd/mempool/mempool_exec"
HOGEXEC_BIN="$BUILD_DIR/hogd/hogexec/hogexec"
EVENT_BIN="$BUILD_DIR/hogd/event/event_exec"
INDEXER_BIN="$BUILD_DIR/hogd/indexer/indexer_exec"
HOGDB_BIN="$BUILD_DIR/hogd/hogdb/hogdb"
AUTH_BIN="$BUILD_DIR/hogd/auth/auth_exec"
CHUCKD_CONFIG="$REPO_ROOT/scripts/config-chuckd.json"

WORK_DIR="$(mktemp -d /tmp/startup_order.XXXXXX)"
declare -A PID_OF
LAUNCHED=()

cleanup() {
  local status=$?
  trap - EXIT INT TERM
  for key in "${LAUNCHED[@]}"; do
    local pid="${PID_OF[$key]:-}"
    [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null || true
  done
  # chuckd forks scpd; reap it by name.
  pkill -f "$(basename "$SCPD_BIN")" 2>/dev/null || true
  pkill -f loadgen_server 2>/dev/null || true
  sleep 1
  for key in "${LAUNCHED[@]}"; do
    local pid="${PID_OF[$key]:-}"
    [[ -n "$pid" ]] && kill -KILL "$pid" 2>/dev/null || true
  done
  rm -rf "$WORK_DIR"
  exit "$status"
}
trap cleanup EXIT INT TERM

# Launch a binary in the background, redirecting its output to a log.
# Usage: spawn <key> <binary> [args...]
#
# The binary is backgrounded directly (NOT wrapped in a `( ... )` subshell),
# so $! is the PID of the binary itself. This matters for cleanup: signaling
# a wrapper subshell would not reach the long-running daemon it launched,
# leaving it alive and holding its fixed port for the next serial scenario.
# The exit status is recovered later via `wait` in the crash check.
spawn() {
  local key="$1"
  shift
  echo "launch $key: $*"
  "$@" >"$WORK_DIR/$key.log" 2>&1 &
  PID_OF[$key]=$!
  LAUNCHED+=("$key")
}

launch() {
  local key="$1"
  case "$key" in
  loadgen)
    spawn loadgen "$LOADGEN_BIN" --bind-ip "$IP" --port "$LOADGEN_PORT" \
      --metrics-port "$LOADGEN_HTTP_PORT"
    ;;
  mempool)
    spawn mempool "$MEMPOOL_BIN" --bind-ip "$IP" --metrics-port "$MEMPOOL_HTTP_PORT" \
      --network-id 0 --num-db-shards 1 --num-exec-shards 1 \
      --db-port "$MEMPOOL_DB_PORT" --event-port "$MEMPOOL_EVENT_PORT" \
      --executor-port "$MEMPOOL_EXECUTOR_PORT" --auth-port "$MEMPOOL_AUTH_PORT" \
      --coordinator-port "$MEMPOOL_COORDINATOR_PORT"
    ;;
  hogexec)
    spawn hogexec "$HOGEXEC_BIN" --coordinator-addr "$IP:$MEMPOOL_EXECUTOR_PORT" \
      --bind-ip "$IP" --nthreads 64 --mempool-port "$EXECUTOR_MEMPOOL_PORT" \
      --metrics-port "$EXECUTOR_HTTP_PORT"
    ;;
  event)
    spawn event "$EVENT_BIN" --mempool-addr "$IP:$MEMPOOL_EVENT_PORT" \
      --bind-ip "$IP" --work-dir "$WORK_DIR/event" \
      --executor-port "$EVENT_EXECUTOR_PORT" --mempool-port "$EVENT_MEMPOOL_PORT" \
      --indexer-port "$EVENT_INDEXER_PORT" --metrics-port "$EVENT_HTTP_PORT"
    ;;
  indexer)
    spawn indexer "$INDEXER_BIN" --event-addr "$IP:$EVENT_INDEXER_PORT" \
      --db-path "$WORK_DIR/indexer" --bind-ip "$IP" \
      --event-port "$INDEXER_EVENT_PORT" \
      --block-explorer-port "$INDEXER_BLOCK_EXPLORER_PORT" \
      --metrics-port "$INDEXER_HTTP_PORT"
    ;;
  hogdb)
    spawn hogdb "$HOGDB_BIN" --db-path "$WORK_DIR/hogdb.data" --ram-bytes 2000000000 \
      --bind-ip "$IP" --coordinator-addr "$IP:$MEMPOOL_DB_PORT" \
      --executor-port "$DB_EXECUTOR_PORT" --mempool-port "$DB_MEMPOOL_PORT" \
      --auth-port "$DB_AUTH_PORT" --metrics-port "$DB_HTTP_PORT"
    ;;
  auth)
    spawn auth "$AUTH_BIN" --mempool-addr "$IP:$MEMPOOL_AUTH_PORT" --bind-ip "$IP" \
      --db-port "$AUTH_DB_PORT" --mempool-port "$AUTH_MEMPOOL_PORT" \
      --metrics-port "$AUTH_HTTP_PORT"
    ;;
  chuckd)
    # num-blocks is deliberately huge: the loadgen drives one block per
    # loop iteration on demand (no upfront allocation), so a large count
    # just keeps the bench running well past the settle window. This is a
    # startup-order test — we want every process up and busy at check time,
    # then killed by cleanup; we do not want the bench to run to completion
    # and tear the coordinator down mid-check.
    spawn chuckd "$CHUCKD_BIN" -l info --stack --config "$CHUCKD_CONFIG" \
      --instance 970c9b --seed 1410441218572558875 --metrics :2223 \
      bench coordinator_loadgen \
      --in-addr "$IP:7888" --debug-port 5500 \
      --num-accts 100 --txs-per-block 1000 --num-blocks 1000000 \
      --storage-dir "$WORK_DIR/chuckd-storage" --out-addr :50000 \
      --loadgen-addr "$IP:$LOADGEN_PORT" \
      --sched-addr "$IP:$MEMPOOL_COORDINATOR_PORT" \
      --scpd-path "$SCPD_BIN" \
      --scp-handle-addr 127.0.0.1:3300 --scp-control-addr 127.0.0.1:4400
    ;;
  *)
    echo "internal error: unknown component $key" >&2
    exit 3
    ;;
  esac
}

# Do NOT pre-create the daemons' own data directories. The indexer's
# IndexDB::open_or_make_new() only creates a fresh DB when its path does NOT
# exist; a pre-existing empty dir sends it down the open() path, which aborts
# with "DB Not Found". Each daemon creates its own dir (matches launch-multi.sh).
mkdir -p "$WORK_DIR/event" "$WORK_DIR/chuckd-storage"

echo "=== scenario $SCENARIO: launching in order: ${ORDER[*]} ==="
for key in "${ORDER[@]}"; do
  launch "$key"
  sleep "$STAGGER_SECS"
done

echo "=== all launched; settling ${SETTLE_SECS}s ==="
sleep "$SETTLE_SECS"

echo "=== crash check ==="
failed=0
for key in "${LAUNCHED[@]}"; do
  pid="${PID_OF[$key]}"
  if kill -0 "$pid" 2>/dev/null; then
    echo "ok    $key (pid $pid still running)"
    continue
  fi
  # Process already exited; recover its status. `wait` on a finished
  # background child of this shell returns that child's exit status.
  status=0
  wait "$pid" 2>/dev/null || status=$?
  if [[ "$status" == "0" ]]; then
    echo "ok    $key (exited cleanly, status 0)"
  else
    echo "FAIL  $key (exit status $status)"
    echo "----- last 50 lines of $key.log -----"
    tail -n 50 "$WORK_DIR/$key.log" || true
    echo "-------------------------------------"
    failed=1
  fi
done

if [[ "$failed" -ne 0 ]]; then
  echo "=== scenario $SCENARIO FAILED ==="
  exit 1
fi
echo "=== scenario $SCENARIO PASSED ==="
