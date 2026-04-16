#!/bin/bash

# tidy-ubuntu.sh - Compatible with standard LLVM run-clang-tidy argument format
# Uses space-separated args instead of equals (e.g., -warnings-as-errors '*' not -warnings-as-errors='*')

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/env.sh
CODE_DIR=hogd
BUILD_DIR=build
FIX_FLAG=""

print_usage() {
  printf "usage: scripts/tidy-ubuntu.sh [OPTION]...\n"
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
set -e
cd ${ROOT_DIR}/hogd/rust/wasmi && cargo clippy --no-deps -- -D warnings
cd ${ROOT_DIR}/hogd/rust/crypto && cargo clippy --no-deps -- -D warnings
cd ${ROOT_DIR}/hogd/rust/wasmtime && cargo clippy --no-deps -- -D warnings
set +e

# Run clang-tidy (using -extra-arg to pass flags to clang-tidy directly)
cd ${ROOT_DIR}
find ${CODE_DIR} -name '*.h' -o -name '*.cc' |
  xargs ${CLANG_TIDY} -j"$NPROC" -use-color -p "${BUILD_DIR}" \
    -clang-tidy-binary /usr/bin/clang-tidy-21 \
    -header-filter="${ROOT_DIR}/hogd/.*" -quiet \
    -extra-arg=-std=c++23 \
    -extra-arg=-stdlib=libc++ \
    -extra-arg=-Werror ${FIX_FLAG}

if [ $? -ne 0 ]; then
  exit 1
fi

cd ${ROOT_DIR}
PYTHON_FILES=$(find hogd/ -name '*.py')
if [ -n "$PYTHON_FILES" ]; then
  printf "Running flake8 on python files\n"
  python3 -m flake8 $PYTHON_FILES
  if [ $? -ne 0 ]; then
    printf "flake8 failed!\n"
    exit 1
  fi
fi
