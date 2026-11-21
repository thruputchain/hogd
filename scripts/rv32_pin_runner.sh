# Sourced by the RV32 cost scripts (rv32_gas_calibrate.sh, rv32_syscall_bench.sh,
# rv32_openssl_speed.sh). rv32_pin_runner sets RUNNER to the command prefix that
# pins a measurement to one core and to a fixed code layout, or exits.
#
# Both used to be best effort: when taskset was missing or `setarch -R` was
# refused, the scripts ran without it and nothing in their output said so.
# Docker's default seccomp profile refuses `setarch -R` (it allows personality()
# only for a handful of values, and ADDR_NO_RANDOMIZE is not one), so a
# calibration run in a container measured a random code layout and reported the
# result as if it were comparable with every run before it. The first one failed
# fibonacci's workload bound at 226.58 ns against 187 or 213 ns on every host run
# for the seven weeks before it.
#
# RV32_ALLOW_UNPINNED=1 runs without whatever pinning is unavailable, with a
# warning, for an exploratory run on a machine that cannot pin. Numbers from it
# are not comparable with a committed reference or with history.

rv32_pin_runner() {
  RUNNER=()
  local problems=()
  local error

  if ! command -v taskset >/dev/null 2>&1; then
    problems+=("one core: taskset not found")
  elif error="$(taskset -c 0 true 2>&1)"; then
    RUNNER+=(taskset -c 0)
  else
    problems+=("one core: taskset -c 0: ${error}")
  fi

  local arch
  arch="$(uname -m)"
  if ! command -v setarch >/dev/null 2>&1; then
    problems+=("a fixed code layout: setarch not found")
  elif error="$(setarch "${arch}" -R true 2>&1)"; then
    RUNNER+=(setarch "${arch}" -R)
  else
    problems+=("a fixed code layout: setarch ${arch} -R: ${error}")
  fi

  if [[ ${#problems[@]} -eq 0 ]]; then
    return 0
  fi

  local problem
  if [[ "${RV32_ALLOW_UNPINNED:-0}" == 1 ]]; then
    for problem in "${problems[@]}"; do
      echo "warning: RV32_ALLOW_UNPINNED=1, measuring without ${problem}" >&2
    done
    echo "warning: these numbers are not comparable with a reference or" \
      "with history" >&2
    return 0
  fi

  echo "error: cannot pin the measurement to" >&2
  for problem in "${problems[@]}"; do
    echo "         ${problem}" >&2
  done
  echo "       Unpinned, the costs move with the code layout and the core: the" >&2
  echo "       gas calibration measured 25 of 100 cells moving over 10%." >&2
  echo "       In a Docker container, setarch -R needs a seccomp profile that" >&2
  echo "       allows personality(ADDR_NO_RANDOMIZE); the default one refuses" >&2
  echo "       it. To measure anyway, set RV32_ALLOW_UNPINNED=1." >&2
  exit 1
}
