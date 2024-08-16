#!/bin/bash

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/..
CODE_DIR=${ROOT_DIR}/hogd
CLANG_TIDY=clang-tidy-18
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
find "${CODE_DIR}" -name '*.h' -o -name '*.cc' |\
xargs ${CLANG_TIDY} -p ${ROOT_DIR}/${BUILD_DIR} -header-filter=".*"\
-extra-arg=-std=gnu++23 --use-color -config-file ${ROOT_DIR}/.clang-tidy -warnings-as-errors='*' 
