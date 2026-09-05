#!/bin/bash

# set up paths
ROOT_DIR=$(realpath ${SCRIPT_DIR}/../)

# define tools
CMAKE=cmake
CLANG_FORMAT=clang-format
CLANG_TIDY=run-clang-tidy
CLANG_CXX=clang++
CLANG_CC=clang
GCC_CXX=g++
GCC_CC=gcc
NPROC=8
if [[ "$OSTYPE" == "darwin"* ]]; then
  NPROC=$(sysctl -n hw.ncpu)
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
  NPROC=$(nproc)
fi
SH_FORMAT=shfmt

# enable custom environment setup if file conf.sh is present
if [ -f ${SCRIPT_DIR}/conf.sh ]; then
  source ${SCRIPT_DIR}/conf.sh
fi

# HOGD_BUILD_JOBS: default parallelism for cmake --build, derived from
# performance-core count and RAM rather than raw core count. Sourced after
# conf.sh so that a pinned NPROC (CI writes one there to bound build memory)
# still wins. NPROC itself is left alone because tidy.sh and dev.sh use it for
# clang-tidy fan-out and container CPU limits.
if [ -f "${SCRIPT_DIR}/build-jobs.sh" ]; then
  source "${SCRIPT_DIR}/build-jobs.sh"
fi
