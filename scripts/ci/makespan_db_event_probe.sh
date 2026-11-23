#!/usr/bin/env bash
# Disposable source experiment for the trusted Event transport dispatch.
set -Eeuo pipefail
umask 077

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

[[ ${GIT_BRANCH:-} == codex/makespan-db-event-transport &&
  ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
  ${HOG_XID:-} =~ ^hogd-event-bench-[1-9][0-9]*-[1-9][0-9]*$ &&
  ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ && $(pwd -P) == /workspace &&
  $(git rev-parse HEAD) == "$GIT_COMMIT" ]] || fail 'Makespan probe requires the pinned container Event dispatch'

. ./scripts/hog-ip.sh

probe_build=release-makespan-db-probe
build_probe() {
  local target=$1
  [[ ! -e $probe_build ]] || fail 'Makespan probe requires a fresh build directory'
  . ./scripts/ci/prepare_env.sh
  set +x
  timeout -k 20 900 ./scripts/build.sh -d "$probe_build" -t Release -c gcc \
    -x -DX23_CORO_TRACE=0 -x -DBUILD_BENCHMARKS=ON -j "$(nproc)" "$target"
}

network_and_storage() {
  local peer=$1 path=$2 address route device mtu wg_device mount cpus
  address=$(hog_ip "$peer")
  route=$(ip -4 route get "$address")
  device=$(awk 'NR == 1 {for (i=1;i<NF;i++) if ($i == "dev") {print $(i+1); exit}}' <<<"$route")
  [[ $device =~ ^[a-z0-9]+$ ]] || fail 'Missing experiment route device'
  mtu=$(cat "/sys/class/net/$device/mtu")
  wg_device="hw$(printf '%s' "$HOG_XID" | sha256sum | cut -c1-12)"
  case "$device:$mtu" in
  "$wg_device:1420") transport=wireguard ;;
  eth0:1424 | eth0:1450 | eth0:1500) transport=overlay ;;
  *) fail "Unsupported experiment route device or MTU: $device/$mtu" ;;
  esac
  mount=$(findmnt -T "$path" -no TARGET,FSTYPE)
  [[ $mount =~ ^/workspace[[:space:]]+(ext4|xfs|btrfs)$ ]] || fail 'Expected disk-backed /workspace bind'
  backing=${BASH_REMATCH[1]}
  cpus=$(awk '/^Cpus_allowed_list:/ {print $2}' /proc/self/status)
  [[ $cpus =~ ^[0-9,-]+$ ]]
  printf 'role=%s source=%s xid=%s transport=%s mtu=%s backing=%s cpus=%s\n' \
    "$HOG_HOST" "$GIT_COMMIT" "$HOG_XID" "$transport" "$mtu" "$backing" "$cpus"
}

if [[ ${1:-} == build-db && $# == 1 && $HOG_HOST == hog11 ]]; then
  build_probe hog-db >&2
  path=$(mktemp -d /workspace/makespan-db.XXXXXXXX)
  [[ $path =~ ^/workspace/makespan-db\.[[:alnum:]]{8}$ ]]
  network_and_storage hog10 "$path"
  printf 'db_path=%s\n' "$path"
  sha256sum "$probe_build/hogd/hog_db/hog-db"
  exit 0
fi

[[ $# == 0 && ${HOG_HOST:-} == hog10 && ${HOGD_EVENT_NETWORK_DIAGNOSTICS:-} == 1 &&
  ${BUILD_DIR:-} == release ]] || fail 'Makespan probe requires an explicit Event transport on hog10'

. ./scripts/ci/bench_harness.sh
mkdir -p .ci/dumps
out=.ci/dumps/makespan-db-event-probe.txt
[[ ! -e $out ]]
network_and_storage hog11 /workspace >"$out"
printf '%s\n' 'branch=codex/makespan-db-event-transport' 'build=gcc Release X23_CORO_TRACE=0 BUILD_BENCHMARKS=ON' >>"$out"
control=./release/hogd/hog_exec/bench/control_cli
ci_wait_control_server "$control" hog11 11111

# The run-local broker uses only the allocated peer through trusted CI TLS.
# The peer command's own build timeout is independent of the client timeout.
peer_log=$(mktemp .ci/tmp/makespan-db-peer.XXXXXXXX)
trap 'rm -f -- "$peer_log"' EXIT
timeout -k 20 960 python3 /opt/hog-ci/peer.py hog11 \
  'bash ./scripts/ci/makespan_db_event_probe.sh build-db' </dev/null >"$peer_log" &
peer_pid=$!
build_probe makespan_runner
wait "$peer_pid"
[[ $(wc -l <"$peer_log") == 3 ]]
grep -Eq "^role=hog11 source=$GIT_COMMIT xid=$HOG_XID transport=$transport mtu=[0-9]+ backing=$backing cpus=[0-9,-]+$" "$peer_log"
db_path=$(sed -n 's/^db_path=//p' "$peer_log")
[[ $db_path =~ ^/workspace/makespan-db\.[[:alnum:]]{8}$ ]]
cat "$peer_log" >>"$out"
runner="./$probe_build/hogd/hog_exec/bench/makespan_runner"
sha256sum "${runner#./}" >>"$out"

cleanup() {
  local status=$?
  trap - EXIT
  rm -f -- "$peer_log"
  # The trusted schedule's after handler also shuts down the control server,
  # which reaps the DB. The workspace remains until trusted container teardown.
  timeout -k 5 25 "$control" hog11 11111 stop
  exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT TERM
db_ip=$(hog_ip hog11)
timeout -k 5 25 "$control" hog11 11111 start \
  "/workspace/$probe_build/hogd/hog_db/hog-db" --db-path "$db_path" \
  --ram-bytes 10000000000 --bind-ip "$db_ip" --executor-port 10000 --mempool-port 10001
timeout -k 5 25 "$control" hog11 11111 expect_running
CI_WAIT_PORT_TIMEOUT_SEC=60
ci_wait_port hog11 10000
ci_wait_port hog11 10001

timeout -k 5 120 "$runner" --db-probe-only seed | grep '^probe_seed: ' >>"$out"
for trial in 1 2 3; do
  printf 'trial=%d\n' "$trial" >>"$out"
  timeout -k 5 120 "$runner" --db-probe-only measure | grep '^probe\(_config\)\?: ' >>"$out"
done
