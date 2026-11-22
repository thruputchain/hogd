#!/bin/bash
# DIAGNOSTIC BRANCH ONLY (geoph/practical-fermi-e4odcp). Not for merge.
#
# Since #4834 (U256Sqrt), vm/GetBlockHeight costs about 26 ns more per call on
# hog10 (100-101 ns against 70-77 in every earlier run), while
# entry/GetBlockHeight and the other crossing rows through the VM did not
# move, and the extra time repeats to within 1 ns across runs. The bench runs
# under `setarch -R`, so every address in the process is fixed by the binary
# and its environment: a placement effect would repeat exactly like that.
# This script asks which placement.
#
# rv32_syscall_bench.sh calls it after the normal measurement, with the same
# binary, host and pinning, so the normal JSON stays comparable with history.
# Everything here goes to stderr (the job log), as "gbh-diag ..." lines.
#
# Stage 1 runs that binary unchanged and moves only data:
#   - which rows run before vm/GetBlockHeight (heap history),
#   - the initial stack pointer, by the size of one environment variable, with
#     argv and environment bytes otherwise held constant across runs,
#   - glibc malloc tunables (heap placement),
#   - ASLR on (stack, heap, mmap and code bases all randomized).
# Stage 2 rebuilds the bench with scripts/rv32_gbh_diag.patch: the entry and
# VM loops each run against a fixture in the row's own stack frame, on the
# heap, and the shared static one (vm/ rows use the first, entry/ rows the
# last); GetBlockHeight's out pointer is moved; and every row prints where its
# handler, snapshot cache, db tip, guest memory and machine landed.
#
# usage: scripts/rv32_gbh_diag.sh BUILD_DIR BINARY

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$1"
BINARY="$2"
SUMMARY="${REPO_ROOT}/scripts/rv32_gbh_diag_summary.py"
PATCH="${REPO_ROOT}/scripts/rv32_gbh_diag.patch"
BENCH_SRC="hogd/hog_exec/runtime/rv32/rv32_syscall_entry_bench.cc"
OUT="${TMPDIR:-/tmp}/gbh-diag"
rm -rf "${OUT}"
mkdir -p "${OUT}"

log() { echo "gbh-diag: $*"; }

# shellcheck source=rv32_pin_runner.sh
source "${REPO_ROOT}/scripts/rv32_pin_runner.sh"
rv32_pin_runner
PINNED=("${RUNNER[@]}")
CORE_ONLY=(taskset -c 0)

# Fewer and shorter than the normal run's 9 x 0.5 s: these rows repeat to well
# under 1% there, and the effect being chased is 25%.
REPS=3
MIN_TIME=0.2s
# Bytes of argv and environment held constant across runs, so that a run's
# initial stack pointer moves only by the shift it asks for. Larger than any
# filter plus any extra variable below.
BASE_PAD=6000

# run_one LABEL FILTER SHIFT KIND [VAR=VALUE...]
#   KIND is "pinned" (taskset + setarch -R, as the normal run) or "aslr"
#   (taskset only). SHIFT moves the initial stack pointer up by that many
#   bytes, by shortening the pad variable.
run_one() {
  local label=$1 filter=$2 shift_bytes=$3 kind=$4
  shift 4
  local used=${#filter}
  local kv
  for kv in "$@"; do
    # The string, its NUL, and its envp slot.
    used=$((used + ${#kv} + 1 + 8))
  done
  local pad_len=$((BASE_PAD - used - shift_bytes))
  if ((pad_len < 1)); then
    log "${label}: pad underflow, skipped"
    return
  fi
  local pad
  pad=$(head -c "${pad_len}" /dev/zero | tr '\0' x)
  local runner=("${PINNED[@]}")
  if [[ ${kind} == aslr ]]; then
    runner=("${CORE_ONLY[@]}")
  fi
  rm -f "${OUT}/run.json"
  local start=${SECONDS}
  env "RV32_GBH_DIAG_PAD=${pad}" "$@" "${runner[@]}" "${BINARY}" \
    --benchmark_filter="${filter}" \
    --benchmark_repetitions="${REPS}" \
    --benchmark_report_aggregates_only=true \
    --benchmark_min_time="${MIN_TIME}" \
    --benchmark_out="${OUT}/run.json" --benchmark_out_format=json \
    >"${OUT}/run.out" 2>"${OUT}/run.err"
  local rc=$?
  cp "${OUT}/run.json" "${OUT}/${label}.json" 2>/dev/null
  cp "${OUT}/run.err" "${OUT}/${label}.err"
  python3 "${SUMMARY}" "${label}" "${OUT}/run.json" "${OUT}/run.err" \
    "${rc} t=$((SECONDS - start))s"
}

# vm/GetBlockHeight minus vm/GetCurrentAddress in the last run, in ns, or nan.
last_gap() {
  python3 - "${OUT}/run.json" <<'EOF'
import json, sys
try:
    rows = json.load(open(sys.argv[1]))["benchmarks"]
except Exception:
    print("nan")
    sys.exit()
m = {r["name"][:-7]: r["ns_per_call"] * 1e9 for r in rows
     if r.get("aggregate_name") == "median" and "ns_per_call" in r}
a, b = m.get("vm/GetBlockHeight"), m.get("vm/GetCurrentAddress")
print("nan" if a is None or b is None else f"{a - b:.1f}")
EOF
}

reproduces() {
  local gap
  gap=$(last_gap)
  log "  gap vm/GetBlockHeight - vm/GetCurrentAddress = ${gap} ns"
  [[ ${gap} != nan ]] && python3 -c "import sys; sys.exit(0 if ${gap} > 12 else 1)"
}

log "start $(date -u +%FT%TZ) host=$(hostname) $(uname -r)"
log "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"
log "glibc: $(ldd --version 2>/dev/null | head -1)"
log "runner: ${PINNED[*]}"
log "binary: $(stat -c %s "${BINARY}") bytes, sha256 $(sha256sum "${BINARY}" | cut -c1-16)"

F_VM='^vm/(GetBlockHeight|GetCurrentAddress)$'
F_EV='^(entry|vm)/(GetBlockHeight|GetCurrentAddress)$'
F_ST='^(entry|vm)/Get'
# The normal run's exact prefix up to and including the stateless rows.
F_RS='^entry/Ristretto255Msm/|^(entry|vm)/Get'

# ---------------------------------------------------------------- stage 1 --
log "== stage 1: the normal run's binary, data moved =="
SWEEP_FILTER=""
for pair in "vm:${F_VM}" "ev:${F_EV}" "st:${F_ST}" "rs:${F_RS}"; do
  name=${pair%%:*}
  filter=${pair#*:}
  run_one "s1-base-${name}" "${filter}" 0 pinned
  if [[ -z ${SWEEP_FILTER} ]] && reproduces; then
    SWEEP_FILTER=${filter}
    log "  reproduces with filter ${name}; it is the sweep's filter"
    # The longer filters only add history; stop at the first that reproduces.
    break
  fi
done
if [[ -z ${SWEEP_FILTER} ]]; then
  log "  did not reproduce under any filter; sweeping with the longest"
  SWEEP_FILTER=${F_RS}
fi

# Fewer points when only a long filter reproduces, to bound the job's time.
case ${SWEEP_FILTER} in
"${F_RS}")
  SHIFTS=(64 1024 4096)
  TUNABLES=(glibc.malloc.tcache_count=0 glibc.malloc.arena_max=1)
  ASLR_RUNS=(1 2)
  ;;
"${F_ST}")
  SHIFTS=(64 256 1024 2048 4096)
  TUNABLES=(glibc.malloc.tcache_count=0 glibc.malloc.mxfast=0
    glibc.malloc.arena_max=1)
  ASLR_RUNS=(1 2 3)
  ;;
*)
  SHIFTS=(16 32 48 64 128 256 512 1024 2048 3072 4096)
  TUNABLES=(glibc.malloc.tcache_count=0 glibc.malloc.mxfast=0
    glibc.malloc.arena_max=1)
  ASLR_RUNS=(1 2 3 4)
  ;;
esac
log "-- stack: initial sp moved up by SHIFT bytes, nothing else changed --"
for s in "${SHIFTS[@]}"; do
  run_one "s1-stack+${s}" "${SWEEP_FILTER}" "${s}" pinned
done

log "-- heap: glibc malloc tunables, stack held where the baseline had it --"
for t in "${TUNABLES[@]}"; do
  run_one "s1-heap-${t#glibc.malloc.}" "${SWEEP_FILTER}" 0 pinned \
    "GLIBC_TUNABLES=${t}"
done

log "-- everything: ASLR on --"
for i in "${ASLR_RUNS[@]}"; do
  run_one "s1-aslr-${i}" "${SWEEP_FILTER}" 0 aslr
done
log "stage 1 done at t=${SECONDS}s"

# ---------------------------------------------------------------- stage 2 --
log "== stage 2: bench rebuilt with ${PATCH#"${REPO_ROOT}"/} =="
if ! git -C "${REPO_ROOT}" apply "${PATCH}"; then
  log "patch did not apply; stage 2 skipped"
  exit 0
fi
build_start=${SECONDS}
if ! cmake --build "${BUILD_DIR}" --parallel "$(nproc)" \
  --target rv32_syscall_entry_bench >"${OUT}/build.log" 2>&1; then
  log "rebuild failed after $((SECONDS - build_start))s; stage 2 skipped"
  grep -E 'error|Error' "${OUT}/build.log" | head -40 | sed 's/^/gbh-diag:   /'
  git -C "${REPO_ROOT}" checkout -- "${BENCH_SRC}"
  exit 0
fi
log "rebuilt in $((SECONDS - build_start))s:" \
  "$(stat -c %s "${BINARY}") bytes, sha256 $(sha256sum "${BINARY}" | cut -c1-16)"

S2_FILTER="${SWEEP_FILTER}|^diag/"
run_one "s2-base" "${S2_FILTER}" 0 pinned RV32_GBH_DIAG_ADDRS=1
run_one "s2-stack+2048" "${S2_FILTER}" 2048 pinned RV32_GBH_DIAG_ADDRS=1
run_one "s2-heap-tcache_count=0" "${S2_FILTER}" 0 pinned \
  RV32_GBH_DIAG_ADDRS=1 GLIBC_TUNABLES=glibc.malloc.tcache_count=0
run_one "s2-aslr-1" "${S2_FILTER}" 0 aslr RV32_GBH_DIAG_ADDRS=1

# Leave the tree as the job checked it out. The binary stays the diag build;
# nothing after the syscall bench runs it.
git -C "${REPO_ROOT}" checkout -- "${BENCH_SRC}"
log "done at t=${SECONDS}s"
