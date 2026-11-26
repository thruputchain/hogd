#!/bin/bash
#
# Configures and builds hogd with PointerTagSanitizer (PTSan); see
# docs/ptsan.md. Takes the options and targets of scripts/build.sh, and builds
# in build/ptsan unless -d names another directory:
#
#   scripts/ptsan/build.sh                 # everything
#   scripts/ptsan/build.sh base_test       # one target
#   scripts/ptsan/build.sh -n              # configure only
#
# The toolchain comes from scripts/ptsan/build-toolchain.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/env.sh"

TOOLCHAIN_BIN="${HOGD_PTSAN_TOOLCHAIN}/bin"
if [ ! -x "${TOOLCHAIN_BIN}/clang++" ] || [ ! -f "${TOOLCHAIN_BIN}/ptsan-glibc.cfg" ]; then
  echo "No PTSan toolchain in ${HOGD_PTSAN_TOOLCHAIN}." >&2
  echo "Build one with scripts/ptsan/build-toolchain.sh, or point" >&2
  echo "HOGD_PTSAN_TOOLCHAIN at an existing build." >&2
  exit 1
fi

# CMakeLists.txt names llvm-ar, llvm-ranlib and (through -fuse-ld=lld) ld.lld
# without a path. They have to be the toolchain's own, not the system LLVM's.
export PATH="${TOOLCHAIN_BIN}:${PATH}"
export CC="${TOOLCHAIN_BIN}/clang"
export CXX="${TOOLCHAIN_BIN}/clang++"

# Benchmarks are off: they add build time, and PTSan timings say nothing about
# hogd's performance.
exec "${SCRIPT_DIR}/../build.sh" -d build/ptsan \
  -x -DHOGD_PTSAN=ON -x -DBUILD_BENCHMARKS=OFF "$@"
