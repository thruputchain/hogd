#!/bin/bash

CMAKE=cmake
CXX=
CXX_FLAGS=
BUILD_DIR=build
BUILD_TYPE=Release

print_usage() {
  printf "usage: scripts/build.sh [OPTION]...\n"
  printf -- "-c sets the compiler (e.g., 'g++', 'clang++')\n"
  printf -- "-f sets compiler flags (e.g., '-Werror')\n"
  printf -- "-d sets the build directory (default is 'build')\n"
  printf -- "-t sets the build type (can be 'Release' or 'Debug')\n"
}

while getopts 'c:f:d:t:' flag; do
  case "${flag}" in
    c) CXX="${OPTARG}" ;;
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

# Find paths
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/../
BUILD_DIR=${ROOT_DIR}/${BUILD_DIR}

# Set nproc
NPROC=$(nproc 2>/dev/null)
#mac compabitibility
if [ $? -ne 0 ]; then
	NPROC=$(sysctl -n hw.ncpu)
fi

# Do the build
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
CXX="${CXX}" $CMAKE -DCMAKE_CXX_FLAGS="${CXX_FLAGS}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  ${CMAKE_INJECT} ..

make -j "$NPROC"
