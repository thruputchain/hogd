#!/bin/bash

# set up paths
ROOT_DIR=$(realpath ${SCRIPT_DIR}/../)

# define tools
CMAKE=cmake
CLANG_FORMAT=clang-format
CLANG_TIDY=run-clang-tidy
CLANG_CXX=clang++
CLANG_CC=clang
GCC_CXX=g++-15
GCC_CC=gcc-15
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
