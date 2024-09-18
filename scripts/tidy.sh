#!/bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh
CODE_DIR=hogd
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

# Run clang-tidy
cd ${ROOT_DIR}
find ${CODE_DIR} -name '*.h' -o -name '*.cc' | \
xargs ${CLANG_TIDY} -j"$NPROC" -use-color -p ${ROOT_DIR}/${BUILD_DIR} \
-header-filter="${ROOT_DIR}/hogd/.*" -quiet -warnings-as-errors='*'
