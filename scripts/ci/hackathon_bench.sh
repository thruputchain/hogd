#!/usr/bin/env bash
set -Eeuo pipefail
: "${BUILD_DIR:?BUILD_DIR must point to the CI build}"
: "${WORK_DIR:?WORK_DIR must point to a fresh benchmark work directory}"
: "${HACKATHON_GATEWAY_BINARY:?absolute compatible hog-gateway path on hog4 required}"
: "${HACKATHON_GATEWAY_SHA256:?pinned gateway sha256 required}"
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
trap retain_results EXIT
python3 "$ROOT/scripts/hackathon/build_contracts.py" --output "$ARTIFACTS"
"$ROOT/scripts/build.sh" -d "$BUILD_DIR" -t Release -x "-DHACKATHON_ARTIFACT_DIR=$ARTIFACTS" hackathon_corpus
CONTROL_PORT=$("$BUILD_DIR/hogd/utils/get_port" kCIControlServerPort)
source "$ROOT/scripts/ci/bench_harness.sh"
export CI_WAIT_CONTROL_TIMEOUT_SEC=${CI_WAIT_CONTROL_TIMEOUT_SEC:-1800}
ci_wait_control_server_hosts "$BUILD_DIR/hogd/hog_exec/bench/control_cli" "$CONTROL_PORT" hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11
GATEWAY_ARGS=()
if [[ -n "${HACKATHON_GATEWAY_PROTOCOL:-}" ]]; then
  GATEWAY_ARGS+=(--gateway-protocol "$HACKATHON_GATEWAY_PROTOCOL")
fi
for shared in 0 1; do
  CORPUS="$WORK_DIR/hackathon-corpus-$shared"
  "$BUILD_DIR/hogd/loadgen/hackathon_corpus" --artifacts "$ARTIFACTS" --out "$CORPUS" --shared-code "$shared"
  python3 "$ROOT/scripts/hackathon/cluster.py" \
    --build "$BUILD_DIR" --scpd "$BUILD_DIR/scpd.dev" \
    --gateway "$HACKATHON_GATEWAY_BINARY" --gateway-sha256 "$HACKATHON_GATEWAY_SHA256" \
    "${GATEWAY_ARGS[@]}" --control-port "$CONTROL_PORT" --corpus "$CORPUS" --output "$RESULTS/code-$shared"
done
