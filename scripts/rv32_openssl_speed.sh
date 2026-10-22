#!/bin/bash
# Run `openssl speed` beside the syscall benchmark, as an independent check on
# the numbers it produces.
#
# Why this is worth having. The syscall benchmark measures hogd's own code, so
# every mistake in the harness -- a call that takes an early-exit path, a loop
# that hoists the work out, a row wired to the wrong arguments -- shows up as a
# plausible number rather than as a failure. One VerifySignature row shipped in
# exactly that state: it verified a signature over the wrong message, took
# ed25519's cheap rejection path, and reported it as the cost of verifying.
# A second implementation of the same primitive, timed on the same host, is
# what catches that class of error.
#
# How tight the check is differs by primitive, and the report says so:
#
#   SHA256 is exact. hogd/crypto/crypto.h calls SHA256_Init/Update/Final, so
#   the syscall and `openssl speed sha256` run the *same* compression function
#   from the same libcrypto. The per-byte terms should agree to within a few
#   percent, and a real gap means the harness is measuring something other than
#   hashing.
#
#   ed25519 is an order-of-magnitude check only. hogd verifies with
#   curve25519-dalek (Rust), OpenSSL 3.0 has its own implementation, and dalek's
#   vectorised backends are severalfold faster, so the tolerance has to allow a
#   factor. What that still catches is a row not doing the work at all -- work
#   hoisted out of the timing loop, a no-op, the wrong syscall -- which lands
#   orders of magnitude out. It does not validate the implementation.
#
#   ML-DSA-65 is the same kind of check as ed25519: hogd verifies with
#   mldsa-native, OpenSSL 3.5+ with its own code. Before 3.5 openssl has no
#   ML-DSA, and the check is skipped rather than failed.
#
# The output is JSON so it can be filed to the metrics history beside the
# benchmark's own, and read back by:
#   scripts/rv32_syscall_report.py sys.json --crosscheck openssl.json
#
# Usage:
#   scripts/rv32_openssl_speed.sh [--out PATH] [--seconds N]

set -euo pipefail

OUT_PATH=""
SECONDS_PER_ALGO=3

usage() {
  echo "usage: $0 [--out PATH] [--seconds N]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --out)
    OUT_PATH="$2"
    shift 2
    ;;
  --out=*)
    OUT_PATH="${1#--out=}"
    shift
    ;;
  --seconds)
    SECONDS_PER_ALGO="$2"
    shift 2
    ;;
  --seconds=*)
    SECONDS_PER_ALGO="${1#--seconds=}"
    shift
    ;;
  *)
    usage
    exit 2
    ;;
  esac
done

if ! command -v openssl >/dev/null 2>&1; then
  echo "error: openssl not found; nothing to cross-check against" >&2
  exit 2
fi

# The same pinning the syscall benchmark uses. A cross-check run on different
# cores, or with a different code layout, is comparing two hosts rather than two
# implementations -- which is the one thing it exists to avoid.
RUNNER=()
if command -v taskset >/dev/null 2>&1; then
  RUNNER=(taskset -c 0)
fi
if command -v setarch >/dev/null 2>&1 &&
  setarch "$(uname -m)" -R true >/dev/null 2>&1; then
  RUNNER+=(setarch "$(uname -m)" -R)
fi

# -mr is openssl's machine-readable mode: one +F line per algorithm carrying
# the rates, rather than a table laid out for humans that has been reformatted
# more than once across releases.
#
# Each algorithm is run separately and allowed to fail on its own. `speed`
# rejects the whole invocation if any named algorithm is unknown, and which
# algorithms exist varies across OpenSSL versions -- so asking for both at once
# would turn a missing ed25519 into a run with no SHA256 either. This is a
# sanity check: it reports what it could measure and stays out of the way of
# what it could not.
#
# ML-DSA-65 exists from OpenSSL 3.5. Older versions reject the name, which is
# the case the per-algorithm loop already absorbs: a warning, and no ML-DSA row
# in the report. Its rates come on a +F<n> line of their own, so that line is
# matched by algorithm name rather than by tag -- and by any spelling of it
# (ML-DSA-65, mldsa65, ml-dsa-65), since a run on OpenSSL 3.6 produced no
# ML-DSA row and no warning: the line was there under a name this filter did
# not expect. A run that succeeds without a recognisable line now says so.
RAW=()
for algo in sha256 ed25519 ML-DSA-65; do
  if output="$("${RUNNER[@]}" openssl speed -seconds "${SECONDS_PER_ALGO}" \
    -mr "${algo}" 2>/dev/null)"; then
    found=0
    while IFS= read -r line; do
      case "${line}" in
      +H:* | +F:* | +F6:*) RAW+=("${line}") ;;
      +F*:*)
        normalized="$(tr -d '-' <<<"${line}" | tr '[:upper:]' '[:lower:]')"
        if [[ "${normalized}" == *:mldsa65:* ]]; then
          RAW+=("${line}")
          found=1
        fi
        ;;
      esac
    done <<<"${output}"
    if [[ "${algo}" == ML-DSA-65 && ${found} -eq 0 ]]; then
      echo "warning: openssl speed ML-DSA-65 ran but printed no ML-DSA-65" \
        "+F line; its -mr lines were:" >&2
      grep -E '^\+' <<<"${output}" | head -5 >&2 || true
    fi
  else
    echo "warning: openssl speed ${algo} failed; skipping it" >&2
  fi
done

if [[ ${#RAW[@]} -eq 0 ]]; then
  echo "error: openssl speed produced no parseable output" >&2
  exit 1
fi

emit() {
  printf '{\n'
  printf '  "openssl_version": "%s",\n' "$(openssl version)"
  # How openssl itself was built, which is half the explanation for any ratio
  # far from 1. A distro libcrypto is typically compiled -march=x86-64
  # -mtune=generic; that does not touch SHA256, whose SHA-NI assembly is
  # dispatched at runtime, but it does bind Ed25519, which has no vectorised
  # backend to dispatch to. Recording it turns a puzzling ratio into a
  # self-explaining one.
  printf '  "openssl_compiler": "%s",\n' \
    "$(openssl version -a | sed -n 's/^compiler: //p' | tr -d '"' |
      cut -c1-400)"
  printf '  "seconds_per_algorithm": %s,\n' "${SECONDS_PER_ALGO}"
  # Which machine this ran on. A ratio between two hosts is not a weaker check
  # than a ratio on one -- it is a misleading one, and it costs real time: a
  # development container migrated to different silicon mid-session, gaining
  # SHA-NI, and a fresh openssl run against a stale benchmark read as a 4x
  # discrepancy in hogd. The report refuses the pair rather than printing it.
  printf '  "cpu_model": "%s",\n' \
    "$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1)"
  printf '  "host": "%s",\n' "$(hostname)"
  printf '  "pinned": "%s",\n' "${RUNNER[*]-}"
  printf '  "raw": [\n'
  local i
  for i in "${!RAW[@]}"; do
    if [[ $i -eq $((${#RAW[@]} - 1)) ]]; then
      printf '    "%s"\n' "${RAW[$i]}"
    else
      printf '    "%s",\n' "${RAW[$i]}"
    fi
  done
  printf '  ]\n}\n'
}

if [[ -n "${OUT_PATH}" ]]; then
  emit >"${OUT_PATH}"
  echo "wrote ${OUT_PATH}" >&2
else
  emit
fi
