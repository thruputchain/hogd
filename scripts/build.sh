#!/bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh

unset NO_MAKE
CXX_FLAGS=
BUILD_DIR=build
BUILD_TYPE=Release
CMAKE_FLAGS=()

print_usage() {
  printf "usage: scripts/build.sh [OPTION]...\n"
  printf -- "-c sets the compiler (e.g., 'gcc', 'clang')\n"
  printf -- "-f sets compiler flags (e.g., '-Werror')\n"
  printf -- "-d sets the build directory (default is 'build')\n"
  printf -- "-t sets the build type (can be 'Release' or 'Debug')\n"
  printf -- "-x sets cmake additional flags"
  printf -- "-n configure but don't run make"
}

while getopts 'c:f:d:t:x:n' flag; do
  case "${flag}" in
  c)
    if [[ "${OPTARG}" == "gcc" ]]; then
      CC="${GCC_CC}"
      CXX="${GCC_CXX}"
    elif [[ "${OPTARG}" == "clang" ]]; then
      CC="${CLANG_CC}"
      CXX="${CLANG_CXX}"
    fi
    ;;
  f) CXX_FLAGS="${OPTARG}" ;;
  d) BUILD_DIR="${OPTARG}" ;;
  t)
    if [[ "${OPTARG}" == "Debug" || "${OPTARG}" == "Release" ]]; then
      BUILD_TYPE="${OPTARG}"
    else
      print_usage
      exit 1
    fi
    ;;
  x) CMAKE_FLAGS+=("${OPTARG}") ;;
  n) NO_MAKE=1 ;;
  *)
    print_usage
    exit 1
    ;;
  esac
done

if [[ "$OSTYPE" == "darwin"* ]]; then
  CMAKE_INJECT=(-DCMAKE_OSX_SYSROOT=)
fi

# Do the build
mkdir -p "$BUILD_DIR"
CC="${CC}" CXX="${CXX}" $CMAKE -DCMAKE_CXX_FLAGS="${CXX_FLAGS}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -B "$BUILD_DIR" \
  "${CMAKE_INJECT[@]}" \
  "${CMAKE_FLAGS[@]}" \
  "$ROOT_DIR"

if test -z "$NO_MAKE"; then
  exec make -j "$NPROC" -C "$BUILD_DIR"
fi
