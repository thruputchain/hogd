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
  # Tell build-jobs.sh whether NPROC below is this file's detected default or a
  # deliberate pin. Comparing values cannot answer that: CI writes lines like
  # `echo 'NPROC=20' >> scripts/conf.sh`, and a runner pinned to exactly its own
  # core count would be indistinguishable from no pin at all. Not exported --
  # build-jobs.sh is sourced into this same shell and unsets it after use, so it
  # never reaches the compiler's environment.
  if grep -qE '^[[:space:]]*(export[[:space:]]+)?NPROC=' ${SCRIPT_DIR}/conf.sh 2>/dev/null; then
    HOGD_NPROC_PINNED=1
  fi
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
