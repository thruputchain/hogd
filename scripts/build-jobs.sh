#!/bin/bash

# Choose a default parallelism for `cmake --build`, exported as HOGD_BUILD_JOBS.
#
# Raw core count is a poor default for this tree:
#
#   * On Apple silicon most logical CPUs are efficiency cores. An M5 Pro reports
#     15 logical CPUs but only 5 performance cores, so a core-count default
#     starts three times more compilers than there are fast cores.
#   * Each C++23 translation unit here peaks near 0.9 GB of resident memory, so
#     parallelism is bounded by RAM as much as by cores.
#
# Measured cold Debug builds on a 24 GB M5 Pro (5 performance + 10 efficiency
# cores), ccache emptied before each run:
#
#   jobs   wall      notes
#   4      12m47s    clean
#   6      11m11s    clean, fastest
#   8      17m42s    3 gtest_discover_tests timeouts under load
#   15     >32m      6.6 GB swapped; total CPU stuck near 470% of 1500%
#
# Precedence, highest first. Anything an operator set explicitly wins over the
# derived value, because CI pins parallelism deliberately to bound build memory
# (see the NPROC lines in .github/workflows/*.yml, written into scripts/conf.sh).
#
#   1. scripts/build.sh -j <n>
#   2. HOGD_BUILD_JOBS=<n>            explicit request
#   3. CI_CPUS=<n>                    container CPU allotment (docker -e)
#   4. NPROC=<n>                      pinned in conf.sh or passed with docker -e,
#                                     i.e. differing from the detected core count
#   5. derived: min(performance cores + 1, RAM / HOGD_GB_PER_JOB), floor 2
#
# HOGD_GB_PER_JOB (default 4) sets the memory budget per job. The margin over
# the ~0.9 GB per-TU peak covers link steps, which are the memory high-water
# mark, and the rest of a working desktop.
#
# This file is sourced by scripts/env.sh (after conf.sh, so pins are visible)
# and by the generated scripts/macos-env.sh, so it must not assume NPROC is
# already set and must stay bash 3.2 compatible.

# _hogd_is_pos_int <value> -- true for a positive decimal integer.
_hogd_is_pos_int() {
  case "${1:-}" in
  '' | *[!0-9]*) return 1 ;;
  0 | 0[0-9]*) return 1 ;;
  *) return 0 ;;
  esac
}

_hogd_sysctl_int() {
  _hogd_val=$(sysctl -n "$1" 2>/dev/null || true)
  if _hogd_is_pos_int "${_hogd_val}"; then
    echo "${_hogd_val}"
  fi
  unset _hogd_val
}

# Detected physical reality, independent of any override.
_hogd_cores=0
_hogd_perf_cores=0
_hogd_mem_gb=0

if [[ "$OSTYPE" == "darwin"* ]]; then
  _hogd_cores=$(_hogd_sysctl_int hw.ncpu)
  _hogd_perf_cores=$(_hogd_sysctl_int hw.perflevel0.logicalcpu)
  _hogd_mem_bytes=$(_hogd_sysctl_int hw.memsize)
  if _hogd_is_pos_int "${_hogd_mem_bytes}"; then
    _hogd_mem_gb=$((_hogd_mem_bytes / 1073741824))
  fi
  unset _hogd_mem_bytes
else
  if command -v nproc >/dev/null 2>&1; then
    _hogd_cores=$(nproc 2>/dev/null || true)
  fi
  # Prefer the cgroup limit: inside a container /proc/meminfo reports the host.
  for _hogd_cg in /sys/fs/cgroup/memory.max /sys/fs/cgroup/memory/memory.limit_in_bytes; do
    if [ -r "${_hogd_cg}" ]; then
      _hogd_cg_val=$(cat "${_hogd_cg}" 2>/dev/null || true)
      if _hogd_is_pos_int "${_hogd_cg_val}"; then
        _hogd_mem_gb=$((_hogd_cg_val / 1073741824))
        break
      fi
    fi
  done
  unset _hogd_cg _hogd_cg_val
  if [ "${_hogd_mem_gb}" = 0 ] && [ -r /proc/meminfo ]; then
    _hogd_mem_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || true)
    if _hogd_is_pos_int "${_hogd_mem_kb}"; then
      _hogd_mem_gb=$((_hogd_mem_kb / 1048576))
    fi
    unset _hogd_mem_kb
  fi
fi
_hogd_is_pos_int "${_hogd_cores}" || _hogd_cores=2

# An explicit HOGD_BUILD_JOBS survives; a value we derived ourselves is
# recomputed, so overrides like HOGD_GB_PER_JOB still work on a re-source.
if [ "${HOGD_BUILD_JOBS_AUTO:-}" = "1" ]; then
  unset HOGD_BUILD_JOBS HOGD_BUILD_JOBS_AUTO
fi

if [ -n "${HOGD_BUILD_JOBS:-}" ]; then
  if ! _hogd_is_pos_int "${HOGD_BUILD_JOBS}"; then
    echo "build-jobs.sh: ignoring HOGD_BUILD_JOBS='${HOGD_BUILD_JOBS}' (not a positive integer)" >&2
    unset HOGD_BUILD_JOBS
  fi
fi

if [ -z "${HOGD_BUILD_JOBS:-}" ] && [ -n "${CI_CPUS:-}" ]; then
  if _hogd_is_pos_int "${CI_CPUS}"; then
    HOGD_BUILD_JOBS=${CI_CPUS}
  else
    echo "build-jobs.sh: ignoring CI_CPUS='${CI_CPUS}' (not a positive integer)" >&2
  fi
fi

# A pinned NPROC differs from the detected core count; that is a deliberate
# bound and must not be raised.
if [ -z "${HOGD_BUILD_JOBS:-}" ] && [ -n "${NPROC:-}" ]; then
  if _hogd_is_pos_int "${NPROC}" && [ "${NPROC}" -ne "${_hogd_cores}" ]; then
    HOGD_BUILD_JOBS=${NPROC}
  fi
fi

if [ -z "${HOGD_BUILD_JOBS:-}" ]; then
  if ! _hogd_is_pos_int "${HOGD_GB_PER_JOB:-}"; then
    HOGD_GB_PER_JOB=4
  fi

  if _hogd_is_pos_int "${_hogd_perf_cores}"; then
    HOGD_BUILD_JOBS=$((_hogd_perf_cores + 1))
    # Never exceed the real core count: on a host with no efficiency cores
    # (some VMs) performance cores equal hw.ncpu, and the +1 would otherwise
    # push the derived value above a bound the operator may have set to exactly
    # the core count.
    if [ "${HOGD_BUILD_JOBS}" -gt "${_hogd_cores}" ]; then
      HOGD_BUILD_JOBS=${_hogd_cores}
    fi
  else
    HOGD_BUILD_JOBS=${_hogd_cores}
  fi

  if [ "${_hogd_mem_gb}" -gt 0 ]; then
    _hogd_mem_jobs=$((_hogd_mem_gb / HOGD_GB_PER_JOB))
    if [ "${_hogd_mem_jobs}" -lt "${HOGD_BUILD_JOBS}" ]; then
      HOGD_BUILD_JOBS=${_hogd_mem_jobs}
    fi
    unset _hogd_mem_jobs
  fi

  if [ "${HOGD_BUILD_JOBS}" -lt 2 ]; then
    HOGD_BUILD_JOBS=2
  fi

  HOGD_BUILD_JOBS_AUTO=1
  export HOGD_BUILD_JOBS_AUTO
fi

export HOGD_BUILD_JOBS
export HOGD_GB_PER_JOB

unset _hogd_cores _hogd_perf_cores _hogd_mem_gb
unset -f _hogd_is_pos_int _hogd_sysctl_int
