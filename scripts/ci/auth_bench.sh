#!/usr/bin/env bash
# Runs the auth server benchmark sweep (hogd/hog_auth/auth_server_bench.cc)
# and summarizes it. Takes about two minutes. Combined Metrics runs it on hog9
# alongside [metrics.sh] through auth_bench_remote.sh, which files the JSON to
# metrics_history as auth_server_bench.
#
#   scripts/ci/auth_bench.sh [BUILD_DIR] [OUT_DIR]
#
# BUILD_DIR defaults to release and must be a Release build with
# BUILD_BENCHMARKS on; OUT_DIR (default auth-bench) receives
# auth_bench.json and auth_bench.md.
#
# Knobs, all optional:
#   AUTH_BENCH_ARGS         extra flags for hog_auth_bench, split on spaces,
#                           e.g. "--auth_bench_in_flight=1,8,32
#                           --auth_bench_batch_sizes=1024"
#   AUTH_BENCH_FILTER       --benchmark_filter regex (default: all)
#   AUTH_BENCH_MIN_TIME     --benchmark_min_time per repetition (default 1s)
#   AUTH_BENCH_REPETITIONS  repetitions per benchmark (default 3)
#   AUTH_BENCH_TIMEOUT      seconds before the run is killed (default 600)
#   AUTH_BENCH_BASELINE     a previous auth_bench.json to diff against
#
# Fails when a benchmark reports an error (a transaction failed to verify),
# never on the numbers themselves: shared runners are too noisy to gate on.
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-release}"
OUT_DIR="${2:-auth-bench}"
BENCH="$BUILD_DIR/hogd/hog_auth/hog_auth_bench"

if [[ ! -x "$BENCH" ]]; then
  echo "auth_bench: $BENCH not found; build hog_auth_bench first" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
JSON="$OUT_DIR/auth_bench.json"
SUMMARY="$OUT_DIR/auth_bench.md"

extra_args=()
if [[ -n "${AUTH_BENCH_ARGS:-}" ]]; then
  read -r -a extra_args <<<"$AUTH_BENCH_ARGS"
fi

args=(
  --benchmark_min_time="${AUTH_BENCH_MIN_TIME:-1s}"
  --benchmark_repetitions="${AUTH_BENCH_REPETITIONS:-3}"
  --benchmark_report_aggregates_only=true
  --benchmark_out="$JSON"
  --benchmark_out_format=json
)
if [[ -n "${AUTH_BENCH_FILTER:-}" ]]; then
  args+=(--benchmark_filter="$AUTH_BENCH_FILTER")
fi
args+=("${extra_args[@]}")

if [[ ${AUTH_BENCH_PLACE_COLD_CASES:-} == 1 &&
      ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
      ${HOG_HOST:-} == hog9 && ( ${GIT_BRANCH:-} == main ||
      ${GIT_BRANCH:-} == codex/auth-cold-all-placement-canary ) &&
      ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
      -z ${AUTH_BENCH_ARGS:-}${AUTH_BENCH_FILTER:-}${AUTH_BENCH_MIN_TIME:-} &&
      -z ${AUTH_BENCH_REPETITIONS:-}${DEFAULT_THREADPOOL_SIZE:-} &&
      ! -v DEFAULT_THREADPOOL_SIZE_hog_auth_bench ]]; then
  timeout --signal=KILL "${AUTH_BENCH_TIMEOUT:-600}" \
    python3 "$ROOT_DIR/scripts/ci/auth_bench_placement.py" "$BENCH" "$JSON" \
    "${args[@]}"
else
  echo "auth_bench: $BENCH ${args[*]}"
  timeout --signal=KILL "${AUTH_BENCH_TIMEOUT:-600}" "$BENCH" "${args[@]}"
fi

status=0
python3 - "$JSON" "$SUMMARY" <<'EOF' || status=$?
import json
import sys

json_path, summary_path = sys.argv[1], sys.argv[2]
with open(json_path, encoding="utf-8") as file:
    data = json.load(file)

errors = []
rows = []
for bench in data.get("benchmarks", []):
    name = bench.get("run_name") or bench.get("name")
    if bench.get("error_occurred"):
        errors.append(f"{name}: {bench.get('error_message', 'error')}")
        continue
    aggregate = bench.get("aggregate_name")
    if bench.get("run_type") == "aggregate" and aggregate not in ("mean", "cv"):
        continue
    rows.append((name, aggregate, bench))

means = {name: b for name, agg, b in rows if agg in (None, "mean")}
cvs = {name: b for name, agg, b in rows if agg == "cv"}


def fmt(value, digits=1):
    if value is None:
        return "-"
    return f"{value:,.{digits}f}"


context = data.get("context", {})
lines = [
    "### Auth server bench",
    "",
    f"{context.get('auth_bench_cores', '?')} "
    f"{'default ' if data.get('auth_bench_placement') else ''}cores, "
    f"{context.get('library_build_type', '?')} benchmark library. "
    "Mean of repetitions; spread is the coefficient of variation of txs/s. "
    "CPU/tx and CPUs busy count the auth server process's CPU time "
    "(verify_warm and verify_saturated only).",
    "",
    "| Benchmark | Time / iter (ms) | txs/s | spread | CPU/tx (us) | CPUs busy "
    "| call p50 (ms) | call p99 (ms) |",
    "|---|---:|---:|---:|---:|---:|---:|---:|",
]
placements = data.get("auth_bench_placement", {})
if placements:
    lines[4:4] = [
        "Cold cases marked † use eight workers on CPUs 0–7.", "",
    ]
for name, bench in means.items():
    cv = cvs.get(name, {}).get("txs_per_s")
    p50 = bench.get("call_p50_us")
    p99 = bench.get("call_p99_us")
    lines.append(
        f"| `{name}`{' †' if name in placements else ''} | {fmt(bench.get('real_time'), 2)} | "
        f"{fmt(bench.get('txs_per_s'), 0)} | "
        f"{'-' if cv is None else f'{cv * 100:.1f}%'} | "
        f"{fmt(bench.get('cpu_us_per_tx'), 2)} | "
        f"{fmt(bench.get('cpus_busy'), 1)} | "
        f"{'-' if p50 is None else fmt(p50 / 1000, 2)} | "
        f"{'-' if p99 is None else fmt(p99 / 1000, 2)} |"
    )
if errors:
    lines += ["", "**Errors:**", ""] + [f"- {e}" for e in errors]

with open(summary_path, "w", encoding="utf-8") as file:
    file.write("\n".join(lines) + "\n")

for error in errors:
    print(f"auth_bench: {error}", file=sys.stderr)
sys.exit(1 if errors or not means else 0)
EOF

if [[ -n "${AUTH_BENCH_BASELINE:-}" && -f "$AUTH_BENCH_BASELINE" ]]; then
  {
    echo
    echo "#### Against baseline"
    echo
    python3 "$ROOT_DIR/scripts/compare_benchmark_json.py" \
      "$AUTH_BENCH_BASELINE" "$JSON"
  } >>"$SUMMARY"
fi

cat "$SUMMARY"
exit "$status"
