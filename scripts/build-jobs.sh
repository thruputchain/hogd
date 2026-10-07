#!/bin/bash

# Choose a default parallelism for `cmake --build`, exported as HOGD_BUILD_JOBS.
#
# Raw core count is a poor default for this tree:
#
#   * On Apple silicon most logical CPUs are efficiency cores. An M5 Pro reports
#     15 logical CPUs but only 5 performance cores, so a core-count default
#     starts three times more compilers than there are fast cores. Intel hybrid
#     laptops (Alder Lake and later) and ARM big.LITTLE have the same split, and
#     on an SMT machine hyperthreads add little to a memory-bound C++ compile.
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
#   4. NPROC=<n>                      pinned in conf.sh or passed with docker -e.
#      env.sh marks conf.sh assignments with HOGD_NPROC_PINNED, since a pin
#      equal to the detected count is indistinguishable by value from no pin.
#      Without the marker (macos-env.sh sources this file directly), compare
#      NPROC with bare nproc's pre-clamp count to avoid mistaking a quota for a pin.
#   5. derived:
#        max(2, min(fast_cores + 1, usable_cores, RAM_GB / HOGD_GB_PER_JOB))
#
#      Fast cores are counted per distinct physical core, filtered by the
#      affinity mask on Linux; macOS uses hw.perflevel0.logicalcpu.
#      usable_cores is nproc/hw.ncpu, clamped on Linux by the cgroup CPU quota.
#
# HOGD_GB_PER_JOB (default 4) sets the memory budget per job. The margin over
# the ~0.9 GB per-TU peak covers link steps, which are the memory high-water
# mark, and the rest of a working desktop.
#
# Set HOGD_BUILD_JOBS_EXPLAIN=1 to print the derivation to stderr; that is the
# first thing to reach for when the value looks wrong on a new machine.
#
# This file is sourced by scripts/env.sh (after conf.sh, so pins are visible)
# and by the generated scripts/macos-env.sh, so it must not assume NPROC is
# already set and must stay bash 3.2 compatible.

# _hogd_is_pos_int <value>: true for a positive decimal integer.
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

# _hogd_mask_size <cpu-list> [mask-list]: how many CPUs of <cpu-list> the mask
# also contains. Both use Linux CPU-list syntax ("0-7,16-23"). An empty mask
# means unrestricted, so the answer is just the size of <cpu-list>.
_hogd_mask_size() {
  awk -v a="${1:-}" -v b="${2:-}" '
    function expand(s, arr,   n, i, se, c, parts) {
      n = split(s, parts, ",")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "") continue
        if (split(parts[i], se, "-") == 2) {
          for (c = se[1] + 0; c <= se[2] + 0; c++) arr[c] = 1
        } else {
          arr[parts[i] + 0] = 1
        }
      }
    }
    BEGIN {
      expand(a, A)
      n = 0
      if (b == "") {
        for (k in A) n++
      } else {
        expand(b, B)
        for (k in A) if (k in B) n++
      }
      print n
    }'
}

# _hogd_mask_intersect <cpu-list> [mask-list]: print the CPUs present in both,
# as a comma-separated list. An empty mask means unrestricted, so <cpu-list> is
# echoed back. Used to narrow the affinity mask down to just the fast cores.
_hogd_mask_intersect() {
  awk -v a="${1:-}" -v b="${2:-}" '
    function expand(s, arr,   n, i, se, c, parts) {
      n = split(s, parts, ",")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "") continue
        if (split(parts[i], se, "-") == 2) {
          for (c = se[1] + 0; c <= se[2] + 0; c++) arr[c] = 1
        } else {
          arr[parts[i] + 0] = 1
        }
      }
    }
    BEGIN {
      expand(a, A)
      if (b != "") { expand(b, B); use = 1 }
      out = ""
      # Walk numerically so the result is ordered and stable.
      for (c = 0; c < 65536; c++) {
        if (!(c in A)) continue
        if (use && !(c in B)) continue
        out = (out == "") ? c : out "," c
      }
      print out
    }'
}

# _hogd_count_cores [mask-list]: read "cpu,core,socket" lines on stdin and count
# distinct physical cores reachable through the mask, since topology is host-wide.
_hogd_count_cores() {
  awk -F, -v b="${1:-}" '
    function expand(s, arr,   n, i, se, c, parts) {
      n = split(s, parts, ",")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "") continue
        if (split(parts[i], se, "-") == 2) {
          for (c = se[1] + 0; c <= se[2] + 0; c++) arr[c] = 1
        } else {
          arr[parts[i] + 0] = 1
        }
      }
    }
    BEGIN { if (b != "") { expand(b, B); use = 1 } }
    /^#/ { next }
    NF >= 3 { if (!use || (($1 + 0) in B)) seen[$2 "," $3] = 1 }
    END { n = 0; for (k in seen) n++; print n }'
}

# Detected physical reality, independent of any override.
_hogd_cores=0
_hogd_fast_cores=0
_hogd_mem_gb=0
_hogd_fast_src=none
_hogd_mem_src=none

if [[ "$OSTYPE" == "darwin"* ]]; then
  _hogd_cores=$(_hogd_sysctl_int hw.ncpu)

  # Use hw.perflevel0.logicalcpu, falling back to all cores (Apple silicon has no SMT).
  _hogd_fast_cores=$(_hogd_sysctl_int hw.perflevel0.logicalcpu)
  _hogd_is_pos_int "${_hogd_fast_cores}" && _hogd_fast_src=perflevel0

  _hogd_mem_bytes=$(_hogd_sysctl_int hw.memsize)
  if _hogd_is_pos_int "${_hogd_mem_bytes}"; then
    _hogd_mem_gb=$((_hogd_mem_bytes / 1073741824))
    _hogd_mem_src=hw.memsize
  fi
  unset _hogd_mem_bytes
else
  if command -v nproc >/dev/null 2>&1; then
    _hogd_cores=$(nproc 2>/dev/null || true)
  fi

  # `docker run --cpus=N` sets a CFS quota; nproc reads the affinity mask and
  # never sees it. Clamp to the quota so a container build does not oversubscribe
  # its own allotment. cgroup v2 first, then v1.
  _hogd_quota=0
  _hogd_period=0
  if [ -r /sys/fs/cgroup/cpu.max ]; then
    _hogd_cpumax=$(cat /sys/fs/cgroup/cpu.max 2>/dev/null || true)
    _hogd_quota=$(echo "${_hogd_cpumax}" | awk '{print $1}')
    _hogd_period=$(echo "${_hogd_cpumax}" | awk '{print $2}')
    unset _hogd_cpumax
  elif [ -r /sys/fs/cgroup/cpu/cpu.cfs_quota_us ]; then
    _hogd_quota=$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us 2>/dev/null || true)
    _hogd_period=$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us 2>/dev/null || true)
  fi
  # Pre-clamp count: the NPROC pin check below compares against bare nproc.
  _hogd_cores_unclamped=${_hogd_cores}

  # "max" (v2) and "-1" (v1) both mean unlimited and fail the integer test.
  if _hogd_is_pos_int "${_hogd_quota}" && _hogd_is_pos_int "${_hogd_period}"; then
    # Round down with a floor of 1, so fractional CPU quotas are not exceeded.
    _hogd_quota_cpus=$((_hogd_quota / _hogd_period))
    [ "${_hogd_quota_cpus}" -lt 1 ] && _hogd_quota_cpus=1
    if _hogd_is_pos_int "${_hogd_cores}" && [ "${_hogd_quota_cpus}" -lt "${_hogd_cores}" ]; then
      _hogd_cores=${_hogd_quota_cpus}
    fi
    unset _hogd_quota_cpus
  fi
  unset _hogd_quota _hogd_period

  # Fast cores, most specific signal first. Every branch intersects with the
  # affinity mask, because sysfs and lscpu describe the whole machine while this
  # process may be pinned to a subset. An empty mask means unrestricted.
  _hogd_mask=$(awk '/^Cpus_allowed_list:/ {print $2}' /proc/self/status 2>/dev/null || true)

  # Step 1: narrow the mask to fast logical CPUs, including SMT siblings;
  # step 2 counts physical cores.
  if [ -r /sys/devices/cpu_core/cpus ]; then
    # Intel hybrid: the kernel splits P-cores and E-cores into separate PMUs,
    # and /sys/devices/cpu_core/cpus lists exactly the P-core CPUs. Pinning a
    # container to E-cores must not report the host's P-cores as available.
    _hogd_cpu_list=$(cat /sys/devices/cpu_core/cpus 2>/dev/null || true)
    _hogd_narrowed=$(_hogd_mask_intersect "${_hogd_cpu_list}" "${_hogd_mask}")
    if [ -n "${_hogd_narrowed}" ]; then
      _hogd_mask=${_hogd_narrowed}
      _hogd_fast_src=cpu_core
    else
      # Pinned entirely to E-cores: keep the full mask so the machine is not idled.
      _hogd_fast_src=cpu_core_unreachable
    fi
    unset _hogd_cpu_list _hogd_narrowed
  elif [ -r /sys/devices/system/cpu/cpu0/cpu_capacity ]; then
    # ARM big.LITTLE: cpu_capacity is normalised to 1024 for the biggest core.
    # Only meaningful when the capacities actually differ.
    _hogd_caps=$(cat /sys/devices/system/cpu/cpu*/cpu_capacity 2>/dev/null | sort -un | tr '\n' ' ')
    if [ "$(echo "${_hogd_caps}" | wc -w | tr -d ' ')" -gt 1 ]; then
      _hogd_maxcap=$(echo "${_hogd_caps}" | tr ' ' '\n' | sort -n | tail -1)
      _hogd_big_list=$(
        for _hogd_c in /sys/devices/system/cpu/cpu[0-9]*; do
          [ -r "${_hogd_c}/cpu_capacity" ] || continue
          if [ "$(cat "${_hogd_c}/cpu_capacity" 2>/dev/null)" = "${_hogd_maxcap}" ]; then
            echo "${_hogd_c##*/cpu}"
          fi
        done | tr '\n' ',' | sed 's/,$//'
      )
      _hogd_narrowed=$(_hogd_mask_intersect "${_hogd_big_list}" "${_hogd_mask}")
      if [ -n "${_hogd_narrowed}" ]; then
        _hogd_mask=${_hogd_narrowed}
        _hogd_fast_src=cpu_capacity
      else
        # Pinned entirely to LITTLE cores; same reasoning as the Intel case.
        _hogd_fast_src=cpu_capacity_unreachable
      fi
      unset _hogd_maxcap _hogd_big_list _hogd_narrowed _hogd_c
    fi
    unset _hogd_caps
  fi

  # Step 2: count distinct physical cores reachable through the mask, deduplicating
  # SMT siblings even with asymmetric SMT or pinning and excluding offline CPUs.
  if command -v lscpu >/dev/null 2>&1; then
    _hogd_fast_cores=$(lscpu -p=CPU,CORE,SOCKET 2>/dev/null |
      _hogd_count_cores "${_hogd_mask}" || true)
    [ "${_hogd_fast_src}" = none ] && _hogd_is_pos_int "${_hogd_fast_cores}" &&
      _hogd_fast_src=lscpu
  fi
  if ! _hogd_is_pos_int "${_hogd_fast_cores}" &&
    [ -r /sys/devices/system/cpu/cpu0/topology/core_id ]; then
    _hogd_fast_cores=$(
      for _hogd_t in /sys/devices/system/cpu/cpu[0-9]*; do
        [ -r "${_hogd_t}/topology/core_id" ] || continue
        echo "${_hogd_t##*/cpu},$(cat "${_hogd_t}/topology/core_id" 2>/dev/null),$(cat "${_hogd_t}/topology/physical_package_id" 2>/dev/null)"
      done | _hogd_count_cores "${_hogd_mask}"
    )
    [ "${_hogd_fast_src}" = none ] && _hogd_is_pos_int "${_hogd_fast_cores}" &&
      _hogd_fast_src=topology
    unset _hogd_t
  fi
  # Neither topology source available, but the mask was narrowed to fast CPUs:
  # fall back to counting those CPUs.
  if ! _hogd_is_pos_int "${_hogd_fast_cores}" && [ "${_hogd_fast_src}" != none ]; then
    _hogd_fast_cores=$(_hogd_mask_size "${_hogd_mask}" "")
  fi
  unset _hogd_mask

  # Prefer the cgroup limit: inside a container /proc/meminfo reports the host.
  for _hogd_cg in /sys/fs/cgroup/memory.max /sys/fs/cgroup/memory/memory.limit_in_bytes; do
    if [ -r "${_hogd_cg}" ]; then
      _hogd_cg_val=$(cat "${_hogd_cg}" 2>/dev/null || true)
      # cgroup v1 reports unlimited as a near-INT64_MAX sentinel; reject values above 1 PiB.
      # cgroup v2's "max" fails the integer test.
      if _hogd_is_pos_int "${_hogd_cg_val}" &&
        [ "${_hogd_cg_val}" -le 1125899906842624 ]; then
        _hogd_mem_gb=$((_hogd_cg_val / 1073741824))
        _hogd_mem_src=cgroup
        break
      fi
    fi
  done
  unset _hogd_cg _hogd_cg_val
  # Key off whether a limit was found, since a small container rounds to 0 GB
  # and must not fall back to the host's RAM.
  if [ "${_hogd_mem_src}" = none ] && [ -r /proc/meminfo ]; then
    _hogd_mem_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || true)
    if _hogd_is_pos_int "${_hogd_mem_kb}"; then
      # MemTotal excludes reserved memory, so round to nearest GiB.
      _hogd_mem_gb=$(((_hogd_mem_kb + 524288) / 1048576))
      _hogd_mem_src=meminfo
    fi
    unset _hogd_mem_kb
  fi
fi
_hogd_is_pos_int "${_hogd_cores}" || _hogd_cores=2
_hogd_is_pos_int "${_hogd_cores_unclamped:-}" || _hogd_cores_unclamped=${_hogd_cores}
if ! _hogd_is_pos_int "${_hogd_fast_cores}"; then
  _hogd_fast_cores=${_hogd_cores}
  _hogd_fast_src=cores
fi

# An explicit HOGD_BUILD_JOBS survives; a value we derived ourselves is
# recomputed, so overrides like HOGD_GB_PER_JOB still work on a re-source.
if [ "${HOGD_BUILD_JOBS_AUTO:-}" = "1" ]; then
  unset HOGD_BUILD_JOBS HOGD_BUILD_JOBS_AUTO
fi

_hogd_jobs_src=derived

if [ -n "${HOGD_BUILD_JOBS:-}" ]; then
  if _hogd_is_pos_int "${HOGD_BUILD_JOBS}"; then
    _hogd_jobs_src=HOGD_BUILD_JOBS
  else
    echo "build-jobs.sh: ignoring HOGD_BUILD_JOBS='${HOGD_BUILD_JOBS}' (not a positive integer)" >&2
    unset HOGD_BUILD_JOBS
  fi
fi

if [ -z "${HOGD_BUILD_JOBS:-}" ] && [ -n "${CI_CPUS:-}" ]; then
  if _hogd_is_pos_int "${CI_CPUS}"; then
    HOGD_BUILD_JOBS=${CI_CPUS}
    _hogd_jobs_src=CI_CPUS
  else
    echo "build-jobs.sh: ignoring CI_CPUS='${CI_CPUS}' (not a positive integer)" >&2
  fi
fi

# Honor pinned NPROC as described in the precedence header.
if [ -z "${HOGD_BUILD_JOBS:-}" ] && [ -n "${NPROC:-}" ] && _hogd_is_pos_int "${NPROC}"; then
  if [ "${HOGD_NPROC_PINNED:-0}" = "1" ] ||
    [ "${NPROC}" -ne "${_hogd_cores_unclamped}" ]; then
    HOGD_BUILD_JOBS=${NPROC}
    _hogd_jobs_src=NPROC
  fi
fi
unset HOGD_NPROC_PINNED

if [ -z "${HOGD_BUILD_JOBS:-}" ]; then
  if ! _hogd_is_pos_int "${HOGD_GB_PER_JOB:-}"; then
    HOGD_GB_PER_JOB=4
  fi

  # Fast cores plus one: the slow cores and the odd stalled job are worth about
  # one extra compiler, never more.
  HOGD_BUILD_JOBS=$((_hogd_fast_cores + 1))

  # Never exceed the usable core count: where fast cores equal the total (no
  # hybrid split, no SMT, or a cgroup-clamped container) the +1 would push the
  # derived value above a bound the operator may have set to exactly that count.
  if [ "${HOGD_BUILD_JOBS}" -gt "${_hogd_cores}" ]; then
    HOGD_BUILD_JOBS=${_hogd_cores}
  fi

  # Apply the RAM bound whenever a memory figure was found, including one that
  # rounds to 0 GB: that correctly collapses to the floor of two jobs rather
  # than skipping the bound entirely.
  if [ "${_hogd_mem_src}" != none ]; then
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

if [ "${HOGD_BUILD_JOBS_EXPLAIN:-0}" = "1" ]; then
  echo "build-jobs.sh: HOGD_BUILD_JOBS=${HOGD_BUILD_JOBS} via ${_hogd_jobs_src}" >&2
  echo "build-jobs.sh:   usable cores=${_hogd_cores} fast cores=${_hogd_fast_cores} (${_hogd_fast_src})" >&2
  echo "build-jobs.sh:   RAM=${_hogd_mem_gb}GB (${_hogd_mem_src}) budget=${HOGD_GB_PER_JOB:-4}GB/job" >&2
fi

export HOGD_BUILD_JOBS
export HOGD_GB_PER_JOB

unset _hogd_cores _hogd_cores_unclamped _hogd_fast_cores _hogd_mem_gb
unset _hogd_fast_src _hogd_mem_src _hogd_jobs_src
unset -f _hogd_is_pos_int _hogd_sysctl_int _hogd_mask_size _hogd_mask_intersect _hogd_count_cores
