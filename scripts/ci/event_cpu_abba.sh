#!/usr/bin/env bash
set -Eeuo pipefail
. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

if [[ ${GIT_BRANCH:-} != codex/event-server-cpu-abba ||
      ${HOGD_CI_CONTAINER:-} != 1 || ${HOGD_CI_OFFLINE:-} != 1 ||
      ${HOG_HOST:-} != hog10 || ${HOGD_EVENT_NETWORK_DIAGNOSTICS:-} != 1 ||
      ! ${HOG_XID:-} =~ ^hogd-event-bench-[0-9]+-[0-9]+$ ||
      ${BUILD_DIR:-} != release || ${HOGD_EVENT_WORK_DIR:-} != /bench-event/ci_work_dir ||
      ${WORK_DIR%/} != "$HOGD_EVENT_WORK_DIR" ]]; then
  echo 'Requires an isolated standalone Event artifact run with the dedicated tmpfs' >&2
  exit 1
fi

. "$(dirname "$0")/../hog-ip.sh"
control=./release/hogd/hog_exec/bench/control_cli
client=./release/hogd/hog_event/hog_event_server_bench
work=/bench-event/ci_work_dir/affinity-arm
out="${HOG_DUMP_DIR:?}/event-cpu-abba"
mkdir -m 700 "$out"
set -o noclobber

remote_proof() {
  # Call sites supply one of the fixed actions and an optional fixed arm label.
  local action=$1
  case "$action" in
    network|mask|reset|'proof stock'|'proof packed') ;;
    *) return 2 ;;
  esac
  timeout -k 5 45 python3 /opt/hog-ci/peer.py hog11 \
    "python3 /workspace/scripts/ci/event_cpu_abba.py $action" </dev/null
}

cleanup_required=0
cleanup() {
  local status=$?
  trap - EXIT
  if [[ $cleanup_required == 1 ]]; then
    timeout -k 5 30 "$control" hog11 11111 stop_checked >>"$out/cleanup.log" 2>&1 || status=1
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

python3 scripts/ci/event_cpu_abba.py network >"$out/hog10-network.json"
remote_proof network >"$out/hog11-network.json"
python3 - "$out" <<'PY'
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
a, b = [json.loads((root / f"{host}-network.json").read_text()) for host in ("hog10", "hog11")]
if not (a["source"] == b["peer"] and b["source"] == a["peer"] and
        a["subnet"] == b["subnet"] and a["mtu"] == b["mtu"]):
    raise SystemExit("Event endpoints are not a matched ordinary run-local pair")
PY

ci_wait_control_server "$control" hog11 11111
remote_proof mask >"$out/topology.json"
mask=$(python3 - "$out/topology.json" <<'PY'
import json, sys
with open(sys.argv[1]) as file:
    mask = json.load(file)["mask"]
if not (len(mask) == 24 and len(set(mask)) == 24 and all(type(cpu) is int and 0 <= cpu < 1024 for cpu in mask)):
    raise SystemExit("invalid packed mask")
print(",".join(map(str, mask)))
PY
)
[[ ${GIT_COMMIT:?} =~ ^[a-f0-9]{40}$ ]]
printf 'source=%s\ntransport=overlay-endpoint\nconnections=40\ncoroutines=100\nevents=1000000\nblocks=20\n' \
  "$GIT_COMMIT" >"$out/settings.txt"

sequence=(stock packed packed stock)
for index in "${!sequence[@]}"; do
  arm=${sequence[index]}
  tag="$((index + 1))-$arm"
  timeout -k 5 30 "$control" hog11 11111 stop_checked >"$out/$tag.pre-stop.log"
  cleanup_required=1
  remote_proof reset >"$out/$tag.reset.json"
  affinity=()
  if [[ $arm == packed ]]; then affinity=(--cores "$mask"); fi
  timeout -k 5 30 "$control" hog11 11111 start "${affinity[@]}" hogd/hog_event/hog-event \
    --mempool-addr "$(hog_ip hog10):8095" --bind-ip "$(hog_ip hog11)" \
    --work-dir "$work" --executor-port 8090 --mempool-port 8091 --indexer-port 8092 \
    >"$out/$tag.start.log"
  ci_wait_port hog11 8090
  ci_wait_port hog11 8091
  remote_proof "proof $arm" >"$out/$tag.before.json"
  timeout -k 5 150 "$client" -e "$(hog_ip hog11):8090" -s "$(hog_ip hog11):8091" \
    -n 1000000 -c 40 -o 100 >"$out/$tag.client.log" 2>"$out/$tag.client.stderr"
  remote_proof "proof $arm" >"$out/$tag.after.json"
  timeout -k 5 30 "$control" hog11 11111 stop_checked >"$out/$tag.stop.log"
  cleanup_required=0
done

python3 scripts/ci/event_cpu_abba_verify.py "$out" >"$out/summary.json"
