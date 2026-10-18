#!/usr/bin/env bash
set -Eeuo pipefail
: "${BUILD_DIR:?BUILD_DIR must point to the CI build}"
: "${WORK_DIR:?WORK_DIR must point to a fresh benchmark work directory}"
: "${HACKATHON_USERS:=1000}"
: "${HACKATHON_RATE_MULTIPLIER:=10}"
: "${HACKATHON_OUTSTANDING:=16384}"
: "${HACKATHON_RECEIPT_WORKERS:=16}"
: "${HACKATHON_GATEWAY_BATCH_SHARDS:=1}"
: "${HACKATHON_GATEWAY_INFLIGHT_BATCHES:=4}"
: "${HACKATHON_REUSE_CORPUS:=0}"
: "${HACKATHON_REPEATS:=3}"
for setting in HACKATHON_USERS:2:10000 HACKATHON_RATE_MULTIPLIER:1:100 HACKATHON_OUTSTANDING:1:131072 HACKATHON_RECEIPT_WORKERS:1:128 HACKATHON_REPEATS:1:10 HACKATHON_GATEWAY_BATCH_SHARDS:1:128 HACKATHON_GATEWAY_INFLIGHT_BATCHES:1:128; do
  IFS=: read -r name minimum maximum <<<"$setting"
  value=${!name}
  if [[ ! "$value" =~ ^[1-9][0-9]{0,5}$ ]] || ((value < minimum || value > maximum)); then
    echo "$name must be an integer between $minimum and $maximum" >&2
    exit 1
  fi
done
if [[ "$HACKATHON_REUSE_CORPUS" != 0 && "$HACKATHON_REUSE_CORPUS" != 1 ]]; then
  echo 'HACKATHON_REUSE_CORPUS must be 0 or 1' >&2
  exit 1
fi
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=$(realpath "$BUILD_DIR")
WORK_DIR=$(realpath -m "$WORK_DIR")
ARTIFACTS="$WORK_DIR/hackathon-artifacts"
RESULTS="$WORK_DIR/hackathon-results"
mkdir -p "$RESULTS"
retain_results() {
  local status=$?
  trap - EXIT
  # Print completed reports even if a later repetition failed. Preserve that
  # failure status while retaining the signed corpora and partial diagnostics.
  python3 - "$RESULTS" <<'PYREPORT' || { [[ "$status" != 0 ]] || status=1; }
import pathlib, sys
for report in sorted(pathlib.Path(sys.argv[1]).glob("code-*/*/report.md")):
    print(f"\n{report}\n{report.read_text()}")
PYREPORT
  if [[ -d "$RESULTS" ]]; then
    local entries=(hackathon-results)
    local corpus
    for corpus in "$WORK_DIR"/hackathon-corpus-*; do
      [[ ! -d "$corpus" ]] || entries+=("$(basename "$corpus")")
    done
    local archive="$WORK_DIR/hackathon-results.tar.gz"
    if tar -czf "$archive" -C "$WORK_DIR" "${entries[@]}"; then
      echo "Hackathon artifact archive: $archive"
      if [[ -n "${HOG_DUMP_DIR:-}" ]]; then
        if mkdir -p "$HOG_DUMP_DIR" && cp "$archive" "$HOG_DUMP_DIR/hackathon-results.tar.gz"; then
          echo "Retained CI artifact: $HOG_DUMP_DIR/hackathon-results.tar.gz"
        else
          [[ "$status" != 0 ]] || status=1
        fi
      fi
    else
      [[ "$status" != 0 ]] || status=1
    fi
  fi
  exit "$status"
}
# Validate restoration before installing the retention trap, so an incompatible
# cache cannot be overwritten by an empty failed run.
if [[ "$HACKATHON_REUSE_CORPUS" == 1 ]]; then
  python3 "$ROOT/hogd/loadgen/hackathon/reuse_corpus.py" \
    --archive "${HOG_DUMP_DIR:?HOG_DUMP_DIR must contain the retained archive}/hackathon-results.tar.gz" \
    --output "$WORK_DIR" --users "$HACKATHON_USERS" --rate-multiplier "$HACKATHON_RATE_MULTIPLIER"
fi
trap retain_results EXIT
if [[ "$HACKATHON_REUSE_CORPUS" == 0 ]]; then
  python3 "$ROOT/hogd/loadgen/hackathon/build_contracts.py" --output "$ARTIFACTS"
  "$ROOT/scripts/build.sh" -d "$BUILD_DIR" -t Release -x "-DHACKATHON_ARTIFACT_DIR=$ARTIFACTS" hackathon_corpus
fi
CONTROL_PORT=$("$BUILD_DIR/hogd/utils/get_port" kCIControlServerPort)
source "$ROOT/scripts/ci/bench_harness.sh"
export CI_WAIT_CONTROL_TIMEOUT_SEC=${CI_WAIT_CONTROL_TIMEOUT_SEC:-1800}
ci_wait_control_server_hosts "$BUILD_DIR/hogd/hog_exec/bench/control_cli" "$CONTROL_PORT" hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11
GATEWAY_BINARY="$BUILD_DIR/hog-gateway"
printf -v gateway_manifest '%q' "$BUILD_DIR/hackathon-gateway.json"
ssh "${CI_SSH_OPTS[@]}" hog4 "cat -- $gateway_manifest" >"$RESULTS/gateway-build.json"
gateway_info=$(
  python3 - "$RESULTS/gateway-build.json" "$ROOT/protocol" <<'PYGATEWAY'
import json, re, subprocess, sys
identity = json.load(open(sys.argv[1]))
protocol = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=sys.argv[2], text=True).strip()
if identity['protocolRevision'] != protocol or identity['protocol'] != 'hogp/' + protocol[:15]:
    raise ValueError('gateway build was not qualified for this hogd protocol')
if not re.fullmatch('[0-9a-f]{64}', identity['sha256']):
    raise ValueError('invalid gateway build checksum')
print(identity['sha256'], identity['protocol'])
PYGATEWAY
)
read -r GATEWAY_SHA256 GATEWAY_PROTOCOL <<<"$gateway_info"
for shared in 0 1; do
  CORPUS="$WORK_DIR/hackathon-corpus-$shared"
  if [[ "$HACKATHON_REUSE_CORPUS" == 0 ]]; then
    "$BUILD_DIR/hogd/loadgen/hackathon/hackathon_corpus" --artifacts "$ARTIFACTS" --out "$CORPUS" --shared-code "$shared" \
      --users "$HACKATHON_USERS" --rate-multiplier "$HACKATHON_RATE_MULTIPLIER"
  fi
  python3 "$ROOT/hogd/loadgen/hackathon/cluster.py" \
    --build "$BUILD_DIR" --scpd "$BUILD_DIR/scpd.dev" \
    --gateway "$GATEWAY_BINARY" --gateway-sha256 "$GATEWAY_SHA256" --gateway-protocol "$GATEWAY_PROTOCOL" \
    --control-port "$CONTROL_PORT" --corpus "$CORPUS" --output "$RESULTS/code-$shared" \
    --outstanding "$HACKATHON_OUTSTANDING" --receipt-workers "$HACKATHON_RECEIPT_WORKERS" --repeats "$HACKATHON_REPEATS" \
    --gateway-batch-shards "$HACKATHON_GATEWAY_BATCH_SHARDS" \
    --gateway-inflight-batches "$HACKATHON_GATEWAY_INFLIGHT_BATCHES"
done
