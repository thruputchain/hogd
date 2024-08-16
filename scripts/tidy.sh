#!/bin/bash

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/..
CODE_DIR=hogd
CLANG_TIDY=run-clang-tidy-18.py
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
xargs ${CLANG_TIDY} -j"$(nproc)" -use-color -p ${ROOT_DIR}/${BUILD_DIR} \
-header-filter=".*" -quiet -warnings-as-errors='*'
