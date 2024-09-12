#!/bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh

CXX_FLAGS=
BUILD_DIR=build
BUILD_TYPE=Release

print_usage() {
  printf "usage: scripts/build.sh [OPTION]...\n"
  printf -- "-c sets the compiler (e.g., 'gcc', 'clang')\n"
  printf -- "-f sets compiler flags (e.g., '-Werror')\n"
  printf -- "-d sets the build directory (default is 'build')\n"
  printf -- "-t sets the build type (can be 'Release' or 'Debug')\n"
}

while getopts 'c:f:d:t:' flag; do
  case "${flag}" in
    c) if [[ "${OPTARG}" == "gcc" ]]; then
         CC="${GCC_CC}"
         CXX="${GCC_CXX}"
       elif [[ "${OPTARG}" == "clang" ]]; then
         CC="${CLANG_CC}"
         CXX="${CLANG_CXX}"
       fi
       ;;
    f) CXX_FLAGS="${OPTARG}" ;;
    d) BUILD_DIR="${OPTARG}" ;;
    t) if [[ "${OPTARG}" == "Debug" || "${OPTARG}" == "Release" ]]
       then
         BUILD_TYPE="${OPTARG}"
       else
         print_usage
         exit 1 
       fi
       ;;
    *) print_usage
       exit 1 ;;
    esac
done

# Find build path
BUILD_DIR=${ROOT_DIR}/${BUILD_DIR}

# Do the build
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
CC="${CC}" CXX="${CXX}" $CMAKE -DCMAKE_CXX_FLAGS="${CXX_FLAGS}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  $(eval echo $CMAKE_INJECT) \
  "$ROOT_DIR"

make -j "$NPROC"
