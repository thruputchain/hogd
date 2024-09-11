#!/bin/bash

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh
ROOT_DIR=${SCRIPT_DIR}/..
CODE_DIR=hogd
if [ -z ${CLANG_TIDY+x} ]; then CLANG_TIDY=run-clang-tidy-18.py; fi
BUILD_DIR=build

print_usage() {
  printf "usage: scripts/tidy.sh [OPTION]...\n"
  printf -- "-d sets the build directory (default is 'build')\n"
}

while getopts 'd:' flag; do
  case "${flag}" in
    d) BUILD_DIR="${OPTARG}" ;;
    *) print_usage
       exit 1 ;;
    esac
done

# Set nproc
NPROC=$(nproc 2>/dev/null)
#mac compabitibility
if [ $? -ne 0 ]; then
	NPROC=$(sysctl -n hw.ncpu)
fi

# Run clang-tidy
cd ${ROOT_DIR}
find ${CODE_DIR} -name '*.h' -o -name '*.cc' | \
xargs ${CLANG_TIDY} -j"$NPROC" -use-color -p ${ROOT_DIR}/${BUILD_DIR} \
-header-filter=".*hogd/hogd/.*" -quiet -warnings-as-errors='*'
