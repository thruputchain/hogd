#!/bin/bash

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/env.sh
CODE_DIR=hogd
BUILD_DIR=build
FIX_FLAG=""

print_usage() {
  printf "usage: scripts/tidy.sh [OPTION]...\n"
  printf -- "-d sets the build directory (default is 'build')\n"
  printf -- "-f automatically fixes clang tidy errors\n"
}

while getopts 'd:f' flag; do
  case "${flag}" in
  d) BUILD_DIR="${OPTARG}" ;;
  f) FIX_FLAG="-fix" ;;
  *)
    print_usage
    exit 1
    ;;
  esac
done

BUILD_DIR=$(readlink -f "$BUILD_DIR")

# Run clippy (serialized with clang-tidy to avoid OOM)
cd ${ROOT_DIR}/hogd/rust/wasmi && cargo clippy --no-deps -- -D warnings || exit 1
cd ${ROOT_DIR}/hogd/rust/wasmtime && cargo clippy --no-deps -- -D warnings || exit 1
cd ${ROOT_DIR}/hogd/rust/crypto && cargo clippy --no-deps -- -D warnings || exit 1
cd ${ROOT_DIR}/hogd/rust/synchronized_map && cargo clippy --no-deps -- -D warnings || exit 1

# Run clang-tidy.
#
# bugprone-exception-escape walks the whole exception-propagation call graph of
# every noexcept function, which is super-linear on the heavily
# template-instantiated WASM contract test/bench TUs: one such file takes 303s
# with the check on vs 53s with it off, and those files alone set the full-tidy
# wall at ~5.5min. The check earns its keep on shipped code (it flags real
# noexcept violations in the syscall handlers), so instead of dropping it
# globally we split the corpus: production TUs get the full check set, while
# test/bench/loadgen TUs and hog-db.cc -- where the check is just as expensive but
# reports nothing -- get everything except exception-escape. The two passes run
# concurrently. Net: full tidy 327s -> 109s with no loss of coverage on
# production code.
cd ${ROOT_DIR}
ALL_FILES=$(find ${CODE_DIR} -name '*.h' -o -name '*.cc')
EE_EXEMPT='_tests?\.(cc|h)$|/contract_tests/|/direct_rpc_tests/|/test_util/|/bench/|/loadgen/|test_framework|/hog_db/hog_db\.cc$'
PROD_FILES=$(echo "$ALL_FILES" | grep -vE "$EE_EXEMPT")
EXEMPT_FILES=$(echo "$ALL_FILES" | grep -E "$EE_EXEMPT")

# clang-tidy fan-out. In the default (non-fix) mode the two passes below run
# concurrently, so -j"$NPROC" each meant twice the core count. clang-tidy peaks
# higher per translation unit than the compiler does, and on an Apple silicon
# laptop most "cores" are efficiency cores: measured on a 24 GB M5 Pro (5
# performance + 10 efficiency cores), -j15 per pass produced 34 concurrent
# clang-tidy processes, 15 GB of swap, and total CPU collapsing from ~1000% to
# ~460% as the machine paged -- a full run had not finished after 48 minutes.
#
# An explicitly pinned parallelism is taken as the per-pass count, so CI is
# unchanged: every workflow writes an NPROC line into scripts/conf.sh to bound
# build memory, and build-jobs.sh passes that through. A value derived from the
# local machine is instead treated as a total budget and split across the two
# concurrent passes.
# A malformed override must be ignored outright, not merely replaced: the
# per-pass halving below keys off HOGD_TIDY_JOBS being set, so a typo would
# otherwise halve a pinned CI parallelism instead of leaving it alone.
case "${HOGD_TIDY_JOBS:-}" in
'') ;;
*[!0-9]* | 0)
  printf "tidy.sh: ignoring HOGD_TIDY_JOBS='%s' (not a positive integer)\n" \
    "${HOGD_TIDY_JOBS}" >&2
  unset HOGD_TIDY_JOBS
  ;;
esac

TIDY_TOTAL_JOBS=${HOGD_TIDY_JOBS:-${HOGD_BUILD_JOBS:-$NPROC}}
case "${TIDY_TOTAL_JOBS}" in
'' | *[!0-9]* | 0) TIDY_TOTAL_JOBS=${NPROC} ;;
esac

if [ -n "$FIX_FLAG" ]; then
  # -fix runs the passes sequentially, so one pass may use the whole budget.
  TIDY_PASS_JOBS=${TIDY_TOTAL_JOBS}
elif [ -n "${HOGD_TIDY_JOBS:-}" ] || [ "${HOGD_BUILD_JOBS_AUTO:-}" = "1" ]; then
  TIDY_PASS_JOBS=$((TIDY_TOTAL_JOBS / 2))
else
  TIDY_PASS_JOBS=${TIDY_TOTAL_JOBS}
fi
if [ "${TIDY_PASS_JOBS}" -lt 1 ]; then
  TIDY_PASS_JOBS=1
fi
printf "Running clang-tidy with -j%s per pass (override with HOGD_TIDY_JOBS)\n" \
  "${TIDY_PASS_JOBS}"

tidy_pass() {
  # Reads file list on stdin; $1 is any extra clang-tidy args for this pass.
  xargs ${CLANG_TIDY} -j"$TIDY_PASS_JOBS" -use-color -p "${BUILD_DIR}" \
    -header-filter="${ROOT_DIR}/hogd/.*" -quiet -warnings-as-errors='*' ${FIX_FLAG} $1
}

if [ -n "$FIX_FLAG" ]; then
  # In -fix mode the passes share header files, so run them sequentially to
  # avoid two clang-tidy processes rewriting the same header at once.
  echo "$PROD_FILES" | tidy_pass ""
  PROD_RC=$?
  echo "$EXEMPT_FILES" | tidy_pass "-checks=-bugprone-exception-escape"
  EXEMPT_RC=$?
else
  echo "$PROD_FILES" | tidy_pass "" &
  PROD_PID=$!
  echo "$EXEMPT_FILES" | tidy_pass "-checks=-bugprone-exception-escape" &
  EXEMPT_PID=$!
  wait $PROD_PID
  PROD_RC=$?
  wait $EXEMPT_PID
  EXEMPT_RC=$?
fi

if [ $PROD_RC -ne 0 ] || [ $EXEMPT_RC -ne 0 ]; then
  exit 1
fi

cd ${ROOT_DIR}
PYTHON_FILES=$(find hogd/ -name '*.py')
if [ -n "$PYTHON_FILES" ]; then
  printf "Running flake8 on python files\n"
  flake8 $PYTHON_FILES
  if [ $? -ne 0 ]; then
    printf "flake8 failed!\n"
    exit 1
  fi
fi
