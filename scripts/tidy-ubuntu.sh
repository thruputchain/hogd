#!/bin/bash

# tidy-ubuntu.sh - Compatible with standard LLVM run-clang-tidy argument format
# Uses space-separated args instead of equals (e.g., -warnings-as-errors '*' not -warnings-as-errors='*')

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/env.sh
CODE_DIR=hogd
BUILD_DIR=build
FIX_FLAG=""
SCOPE="full"
PARTITION="all"
BASE_SHA=""
HEAD_SHA=""
CLANG_TIDY_BINARY="${CLANG_TIDY_BINARY:-/usr/bin/clang-tidy-21}"

print_usage() {
  printf "usage: scripts/tidy-ubuntu.sh [OPTION]...\n"
  printf -- "-d sets the build directory (default is 'build')\n"
  printf -- "-f automatically fixes clang tidy errors\n"
  printf -- "--scope full|changed selects whole-repo or changed-file tidy (default: full)\n"
  printf -- "--partition all|prod|exempt|other selects the tidy partition (default: all)\n"
  printf -- "--base sets the base git SHA for changed scope\n"
  printf -- "--head sets the head git SHA for changed scope\n"
  printf -- "CLANG_TIDY_BINARY overrides the clang-tidy binary (default: /usr/bin/clang-tidy-21)\n"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  -d)
    BUILD_DIR="$2"
    shift 2
    ;;
  -f)
    FIX_FLAG="-fix"
    shift
    ;;
  --scope)
    SCOPE="$2"
    shift 2
    ;;
  --partition)
    PARTITION="$2"
    shift 2
    ;;
  --base)
    BASE_SHA="$2"
    shift 2
    ;;
  --head)
    HEAD_SHA="$2"
    shift 2
    ;;
  -h | --help)
    print_usage
    exit 0
    ;;
  *)
    print_usage
    exit 1
    ;;
  esac
done

BUILD_DIR=$(readlink -f "$BUILD_DIR")

if [[ "$SCOPE" != "full" && "$SCOPE" != "changed" ]]; then
  printf "unsupported scope: %s\n" "$SCOPE" >&2
  exit 1
fi

if [[ "$PARTITION" != "all" && "$PARTITION" != "prod" &&
  "$PARTITION" != "exempt" && "$PARTITION" != "other" ]]; then
  printf "unsupported partition: %s\n" "$PARTITION" >&2
  exit 1
fi

if [[ "$SCOPE" == "changed" && (-z "$BASE_SHA" || -z "$HEAD_SHA") ]]; then
  printf "--base and --head are required with --scope changed\n" >&2
  exit 1
fi

run_other_checks() {
  # Run clippy separately from clang-tidy to avoid OOM.
  set -e
  cd ${ROOT_DIR}/hogd/rust/crypto && cargo clippy --no-deps -- -D warnings
  cd ${ROOT_DIR}
  PYTHON_FILES=$(find hogd/ -name '*.py')
  if [ -n "$PYTHON_FILES" ]; then
    printf "Running flake8 on python files\n"
    python3 -m flake8 $PYTHON_FILES
  fi
  set +e
}

tidy_inputs() {
  local partition="$1"
  local proxy_root="$2"
  local args=(
    "${SCRIPT_DIR}/tidy_inputs.py"
    --compile-db "${BUILD_DIR}/compile_commands.json"
    --scope "$SCOPE"
    --partition "$partition"
    --proxy-dir "${proxy_root}/proxies"
    --proxy-compile-db "${proxy_root}/compile_commands.json"
  )
  if [[ "$SCOPE" == "changed" ]]; then
    args+=(--base "$BASE_SHA" --head "$HEAD_SHA")
  fi
  python3 "${args[@]}"
}

run_cpp_tidy() {
  local partition="$1"
  shift
  local extra_args=("$@")
  local input_file
  local proxy_root
  input_file=$(mktemp)
  proxy_root=$(mktemp -d)
  tidy_inputs "$partition" "$proxy_root" >"$input_file" || {
    rm -f "$input_file"
    rm -rf "$proxy_root"
    return 1
  }
  local input_count
  input_count=$(wc -l <"$input_file")
  if [[ "$input_count" -eq 0 ]]; then
    printf "No clang-tidy inputs for %s/%s\n" "$SCOPE" "$partition"
    rm -f "$input_file"
    rm -rf "$proxy_root"
    return 0
  fi
  printf "Running clang-tidy for %s/%s on %s inputs\n" "$SCOPE" "$partition" "$input_count"
  xargs -r ${CLANG_TIDY} -j"$NPROC" -use-color -p "${proxy_root}" \
    -clang-tidy-binary "$CLANG_TIDY_BINARY" \
    -header-filter="${ROOT_DIR}/hogd/.*" -quiet \
    -extra-arg=-std=c++23 \
    -extra-arg=-stdlib=libc++ \
    -extra-arg=-Werror ${FIX_FLAG} "${extra_args[@]}" <"$input_file"
  local rc=$?
  rm -f "$input_file"
  rm -rf "$proxy_root"
  return "$rc"
}

cd ${ROOT_DIR}
case "$PARTITION" in
other)
  run_other_checks
  ;;
prod)
  run_cpp_tidy prod
  ;;
exempt)
  run_cpp_tidy exempt -checks=-bugprone-exception-escape
  ;;
all)
  if [ -n "$FIX_FLAG" ]; then
    run_cpp_tidy prod
    PROD_RC=$?
    run_cpp_tidy exempt -checks=-bugprone-exception-escape
    EXEMPT_RC=$?
  else
    run_cpp_tidy prod &
    PROD_PID=$!
    run_cpp_tidy exempt -checks=-bugprone-exception-escape &
    EXEMPT_PID=$!
    wait $PROD_PID
    PROD_RC=$?
    wait $EXEMPT_PID
    EXEMPT_RC=$?
  fi
  run_other_checks
  OTHER_RC=$?
  if [ $PROD_RC -ne 0 ] || [ $EXEMPT_RC -ne 0 ] || [ $OTHER_RC -ne 0 ]; then
    exit 1
  fi
  ;;
esac
