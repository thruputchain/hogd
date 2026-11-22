# DIAGNOSTIC BRANCH ONLY (geoph/practical-fermi-e4odcp). Not for merge.
#
# Follow-up to rv32_gbh_diag.sh. That run found vm/GetBlockHeight at 103.7 ns
# in the normal measurement and 74.4-76.9 ns in ten reruns of the same binary,
# but every rerun differed from the normal invocation in three ways at once:
# its stack sat ~6 KB lower, it ran 3 x 0.2 s instead of 9 x 0.5 s, and it was
# not the first process after the build. This one starts from a twin of the
# normal invocation and changes one thing at a time.
#
# Sourced by rv32_syscall_bench.sh in a subshell, right after the normal run,
# so the environment is the normal run's byte for byte (no new shell, so no
# SHLVL change; the binary is started the same way, so the same `_`). It reads
# that script's BINARY, ARGS, RUNNER, BUILD_DIR and REPO_ROOT. Output: stderr,
# "gbh2 ..." lines.
#
# A twin has the normal run's argc and argv byte count: the filter's bytes are
# taken back out of the --benchmark_out path, and --benchmark_out_format is
# dropped (json is the default) to make room for the filter, so the initial
# stack pointer is the normal run's. A stack shift exports one variable, Z,
# whose string and envp slot move the stack down by `shift` bytes; nothing in
# the process copies the environment, so the heap does not move with it.

set -uo pipefail

G2_OUT=/tmp/gbh2
rm -rf "${G2_OUT}"
mkdir -p "${G2_OUT}"
G2_SUMMARY="${REPO_ROOT}/scripts/rv32_gbh_diag2_summary.py"
G2_PATCH="${REPO_ROOT}/scripts/rv32_gbh_diag.patch"
G2_BENCH_SRC="hogd/hog_exec/runtime/rv32/rv32_syscall_entry_bench.cc"
G2_SLOW_GAP=12

g2log() { echo "gbh2: $*"; }

G2_OUT_FLAG="--benchmark_out="

G2_NORMAL_BYTES=0
for a in "${ARGS[@]}"; do
  G2_NORMAL_BYTES=$((G2_NORMAL_BYTES + ${#a} + 1))
done
G2_NORMAL_ARGC=${#ARGS[@]}

# A path of exactly $1 characters under /tmp, with its directory created.
g2_out_path() {
  python3 - "$1" <<'EOF'
import os, sys
n = int(sys.argv[1])
r = n - len("/tmp/")
if r < 1:
    sys.exit(1)
comps = []
while r > 200:
    k = min(200, r - 2)
    comps.append("d" * k)
    r -= k + 1
comps.append("f" * r)
path = "/tmp/" + "/".join(comps)
os.makedirs(os.path.dirname(path), exist_ok=True)
print(path)
EOF
}

# g2_run LABEL FILTER REPS MIN_TIME SHIFT [BINARY [VAR=VALUE]]
# Prints one summary line; leaves the gap in G2_GAP (nan if unmeasured).
g2_run() {
  local label=$1 filter=$2 reps=$3 min_time=$4 shift_bytes=$5
  local binary=${6:-${BINARY}} extra=${7:-}
  local fixed=("--benchmark_filter=${filter}"
    "--benchmark_repetitions=${reps}"
    "--benchmark_report_aggregates_only=true"
    "--benchmark_min_time=${min_time}")
  local used=0 a
  for a in "${fixed[@]}"; do
    used=$((used + ${#a} + 1))
  done
  local out_len=$((G2_NORMAL_BYTES - used - ${#G2_OUT_FLAG} - 1))
  if ((${#fixed[@]} + 1 != G2_NORMAL_ARGC)) || ((out_len < 6)); then
    g2log "${label}: cannot twin the normal argv (argc ${G2_NORMAL_ARGC}," \
      "out path ${out_len}); skipped"
    G2_GAP=nan
    return
  fi
  local out
  out=$(g2_out_path "${out_len}") || {
    g2log "${label}: no out path"
    G2_GAP=nan
    return
  }
  rm -f "${out}"
  local start=${SECONDS}
  (
    if ((shift_bytes > 0)); then
      # "Z=" + value + NUL + the envp slot.
      local pad_len=$((shift_bytes - 2 - 1 - 8))
      export Z
      Z=$(head -c "${pad_len}" /dev/zero | tr '\0' z)
    fi
    if [[ -n ${extra} ]]; then
      export "${extra?}"
    fi
    "${RUNNER[@]}" "${binary}" "${fixed[@]}" "--benchmark_out=${out}" \
      >"${G2_OUT}/run.out" 2>"${G2_OUT}/run.err"
  )
  local rc=$?
  cp "${out}" "${G2_OUT}/${label}.json" 2>/dev/null
  G2_GAP=$(python3 "${G2_SUMMARY}" "${label}" "${out}" "${G2_OUT}/run.err" \
    "${rc} t=$((SECONDS - start))s" 3>&1 1>&2)
  rm -f "${out}"
}

g2_slow() { [[ ${G2_GAP} != nan ]] && python3 -c "import sys; sys.exit(0 if ${G2_GAP} > ${G2_SLOW_GAP} else 1)"; }

# The rows the normal run executes up to vm/GetBlockHeight (Ristretto255Msm,
# then entry/ and vm/ GetBlockHeight), plus GetCurrentAddress after it as the
# control. Only Ristretto rows start with entry/R.
F_A='^entry/R|^(entry|vm)/Get(Bl|Cu)'
F_EV='^(entry|vm)/Get(Bl|Cu)'
F_VM='^vm/Get(Bl|Cu)'

g2log "start $(date -u +%FT%TZ); normal argv ${G2_NORMAL_ARGC} args," \
  "${G2_NORMAL_BYTES} bytes; binary sha256 $(sha256sum "${BINARY}" | cut -c1-16)"

# ---- step 1: twins at the normal stack, one variable at a time -------------
g2log "== step 1: twins of the normal invocation =="
g2_run A-twin-rs-9x0.5 "${F_A}" 9 0.5s 0
A_SLOW=no
g2_slow && A_SLOW=yes
g2_run B-twin-rs-3x0.2 "${F_A}" 3 0.2s 0
B_SLOW=no
g2_slow && B_SLOW=yes
g2_run D3-twin-vm-9x0.5 "${F_VM}" 9 0.5s 0
D3_SLOW=no
g2_slow && D3_SLOW=yes
g2_run D2-twin-ev-3x0.2 "${F_EV}" 3 0.2s 0
D2_SLOW=no
g2_slow && D2_SLOW=yes
g2_run D1-twin-vm-3x0.2 "${F_VM}" 3 0.2s 0
D1_SLOW=no
g2_slow && D1_SLOW=yes
g2log "slow at the normal stack: A=${A_SLOW} B=${B_SLOW} D3=${D3_SLOW}" \
  "D2=${D2_SLOW} D1=${D1_SLOW}"

# The cheapest twin that reproduces carries the stack sweep.
SWEEP=()
STEP=16
if [[ ${D1_SLOW} == yes ]]; then
  SWEEP=("${F_VM}" 3 0.2s)
elif [[ ${D2_SLOW} == yes ]]; then
  SWEEP=("${F_EV}" 3 0.2s)
elif [[ ${D3_SLOW} == yes ]]; then
  SWEEP=("${F_VM}" 9 0.5s)
  STEP=64
elif [[ ${B_SLOW} == yes ]]; then
  SWEEP=("${F_A}" 3 0.2s)
  STEP=512
elif [[ ${A_SLOW} == yes ]]; then
  SWEEP=("${F_A}" 9 0.5s)
  STEP=1024
fi

if ((${#SWEEP[@]} == 0)); then
  # No twin reproduced: either the full row set matters, or being the first
  # process after the build does. The normal invocation once more, unfiltered,
  # with an out path of the same length, tells which.
  g2log "== step 2: no twin reproduced; the normal invocation again =="
  if [[ ${ARGS[3]} != "${G2_OUT_FLAG}"* ]] || ((G2_NORMAL_ARGC != 5)); then
    g2log "normal argv is not the expected shape; done"
    exit 0
  fi
  normal_out=$(g2_out_path "$((${#ARGS[3]} - ${#G2_OUT_FLAG}))")
  start=${SECONDS}
  ("${RUNNER[@]}" "${BINARY}" "${ARGS[0]}" "${ARGS[1]}" "${ARGS[2]}" \
    "--benchmark_out=${normal_out}" "${ARGS[4]}" \
    >"${G2_OUT}/run.out" 2>"${G2_OUT}/run.err")
  rc=$?
  python3 "${G2_SUMMARY}" E-normal-again "${normal_out}" "${G2_OUT}/run.err" \
    "${rc} t=$((SECONDS - start))s" 3>/dev/null
  g2log "done at t=${SECONDS}s"
  exit 0
fi

# ---- step 2: stack sweep from the normal stack, heap held ------------------
g2log "== step 2: stack moved down by SHIFT bytes, filter ${SWEEP[0]}" \
  "${SWEEP[1]}x${SWEEP[2]}, step ${STEP} =="
for ((shift_bytes = STEP; shift_bytes < 4096; shift_bytes += STEP)); do
  g2_run "S-shift-${shift_bytes}" "${SWEEP[@]}" "${shift_bytes}"
done
g2log "sweep done at t=${SECONDS}s"

# ---- step 3: the same sweep on the address-dump build ----------------------
# Only when the sweep is cheap: it says where the handler, guest memory, machine
# and stack sit at the slow shifts and at their fast neighbours.
if ((STEP != 16)); then
  g2log "done at t=${SECONDS}s (no address sweep at step ${STEP})"
  exit 0
fi
g2log "== step 3: rebuild with ${G2_PATCH#"${REPO_ROOT}"/}, address sweep =="
if ! git -C "${REPO_ROOT}" apply "${G2_PATCH}"; then
  g2log "patch did not apply; done"
  exit 0
fi
build_start=${SECONDS}
if ! cmake --build "${BUILD_DIR}" --parallel "$(nproc)" \
  --target rv32_syscall_entry_bench >"${G2_OUT}/build.log" 2>&1; then
  g2log "rebuild failed after $((SECONDS - build_start))s"
  grep -E 'error' "${G2_OUT}/build.log" | head -20 | sed 's/^/gbh2:   /'
  git -C "${REPO_ROOT}" checkout -- "${G2_BENCH_SRC}"
  exit 0
fi
g2log "rebuilt in $((SECONDS - build_start))s"
# RV32_GBH_DIAG_ADDRS adds the same bytes to every run, so it moves the whole
# sweep by one fixed offset; the sweep covers the page either way.
for ((shift_bytes = 0; shift_bytes < 4096; shift_bytes += STEP)); do
  g2_run "P-shift-${shift_bytes}" "${SWEEP[@]}" "${shift_bytes}" "${BINARY}" \
    RV32_GBH_DIAG_ADDRS=1
done
git -C "${REPO_ROOT}" checkout -- "${G2_BENCH_SRC}"
g2log "done at t=${SECONDS}s"
