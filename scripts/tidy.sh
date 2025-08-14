#!/bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
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
    *) print_usage
       exit 1 ;;
    esac
done


# Run clippy
cd ${ROOT_DIR}/hogd/hogexec/wasmi
cargo +nightly clippy --no-deps -- -D warnings &
CLIPPY_PID=$!

# Run clang-tidy
cd ${ROOT_DIR}
find ${CODE_DIR} -name '*.h' -o -name '*.cc' | \
xargs ${CLANG_TIDY} -j"$NPROC" -use-color -p ${ROOT_DIR}/${BUILD_DIR} \
-header-filter="${ROOT_DIR}/hogd/.*" -quiet -warnings-as-errors='*' ${FIX_FLAG}

if [ $? -ne 0 ]; then
  exit 1
fi

wait $CLIPPY_PID
CLIPPY_RESULT=$?
if [ $CLIPPY_RESULT -ne 0 ]; then
  printf "clippy failed!\n"
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




