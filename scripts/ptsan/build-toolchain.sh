#!/bin/bash
#
# Builds the PointerTagSanitizer (PTSan) toolchain that hogd's PTSan mode
# compiles with: Aarno Labs' fork of LLVM, with clang, lld, the PTSan runtime,
# and the libc++ built to accept tagged pointers. See docs/ptsan.md.
#
# The build is a full clang and takes an hour or two on a few cores. It is
# incremental: rerunning continues where a previous run stopped.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/env.sh"

print_usage() {
  printf "usage: scripts/ptsan/build-toolchain.sh [OPTION]...\n"
  printf -- "  -s sets the PTSan source directory (default '%s');\n" "$HOGD_PTSAN_SRC"
  printf -- "     cloned from %s when missing\n" "$HOGD_PTSAN_REPO"
  printf -- "  -b sets the toolchain build directory (default '%s')\n" "$HOGD_PTSAN_TOOLCHAIN"
  printf -- "  -r sets the PTSan revision to clone (default '%s')\n" "$HOGD_PTSAN_REF"
  printf -- "  -j sets the number of build jobs (default: all cores)\n"
  printf -- "  -n configure but don't run the build\n"
}

JOBS=
unset NO_BUILD
while getopts 's:b:r:j:n' flag; do
  case "${flag}" in
  s) HOGD_PTSAN_SRC="${OPTARG}" ;;
  b) HOGD_PTSAN_TOOLCHAIN="${OPTARG}" ;;
  r) HOGD_PTSAN_REF="${OPTARG}" ;;
  j) JOBS="${OPTARG}" ;;
  n) NO_BUILD=1 ;;
  *)
    print_usage
    exit 1
    ;;
  esac
done

if [ ! -d "${HOGD_PTSAN_SRC}/llvm" ]; then
  git clone --depth 1 --branch "${HOGD_PTSAN_REF}" "${HOGD_PTSAN_REPO}" "${HOGD_PTSAN_SRC}"
fi
echo "PTSan source: ${HOGD_PTSAN_SRC} at $(git -C "${HOGD_PTSAN_SRC}" rev-parse --short=12 HEAD)"

# Fixes hogd needs on top of that release; each patch says what it fixes.
for patch in "${SCRIPT_DIR}"/patches/*.patch; do
  if git -C "${HOGD_PTSAN_SRC}" apply --reverse --check "${patch}" 2>/dev/null; then
    echo "already applied: $(basename "${patch}")"
  else
    git -C "${HOGD_PTSAN_SRC}" apply "${patch}"
    echo "applied: $(basename "${patch}")"
  fi
done

# PTSan.cmake is upstream's configuration. On top of it:
#  - lld, because hogd links with -fuse-ld=lld and the linker has to match the
#    compiler;
#  - only the host backend, and no static analyzer, to shorten the build.
HOST_ARCH="$(uname -m)"
case "${HOST_ARCH}" in
x86_64) LLVM_TARGET=X86 ;;
aarch64) LLVM_TARGET=AArch64 ;;
*)
  echo "PTSan supports x86-64 and AArch64 Linux, not ${HOST_ARCH}" >&2
  exit 1
  ;;
esac

# The host compiler only matters to a fresh build directory; CMake keeps the
# one it first configured with. CC and CXX choose it. Without them, an
# installed clang and lld are used: they are what hogd's own clang builds need
# anyway, and lld links the LLVM tools in a fraction of GNU ld's memory.
HOST_ARGS=()
if [ ! -f "${HOGD_PTSAN_TOOLCHAIN}/CMakeCache.txt" ] && [ -z "${CC:-}${CXX:-}" ] &&
  command -v clang >/dev/null && command -v clang++ >/dev/null; then
  HOST_ARGS+=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
  if command -v ld.lld >/dev/null; then
    HOST_ARGS+=(-DLLVM_USE_LINKER=lld)
  fi
fi

cmake -S "${HOGD_PTSAN_SRC}/llvm" -B "${HOGD_PTSAN_TOOLCHAIN}" -G Ninja \
  -C "${HOGD_PTSAN_SRC}/clang/cmake/caches/PTSan.cmake" \
  "${HOST_ARGS[@]}" \
  -DLLVM_ENABLE_PROJECTS="clang;compiler-rt;lld" \
  -DLLVM_TARGETS_TO_BUILD="${LLVM_TARGET}" \
  -DLLVM_PARALLEL_LINK_JOBS=2 \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DLLVM_APPEND_VC_REV=OFF

if test -n "${NO_BUILD:-}"; then
  exit 0
fi

# ptsan-dependencies is the libc++, libc++abi and libunwind that the generated
# ptsan-glibc*.cfg files select. The LLVM binutils are the ones hogd's
# CMakeLists.txt names (llvm-ar, llvm-ranlib) plus what debugging needs.
cmake --build "${HOGD_PTSAN_TOOLCHAIN}" ${JOBS:+--parallel "${JOBS}"} --target \
  clang lld ptsan ptsan-dependencies \
  llvm-ar llvm-ranlib llvm-nm llvm-objdump llvm-objcopy llvm-strip \
  llvm-symbolizer llvm-config

echo "PTSan toolchain: ${HOGD_PTSAN_TOOLCHAIN}"
"${HOGD_PTSAN_TOOLCHAIN}/bin/clang" --version | head -1
