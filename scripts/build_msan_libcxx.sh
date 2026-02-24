#!/bin/bash
#
# Build an MSan-instrumented libc++ for use with MemorySanitizer.
# This only needs to be run once per LLVM version / machine.
#
# Usage:
#   ./scripts/build_msan_libcxx.sh [INSTALL_DIR]
#
# Default install: $HOME/msan-libcxx
#
# After running this, build hogd with MSan:
#   MSAN_LIBCXX=$HOME/msan-libcxx ./scripts/build.sh \
#     -d build/llvm_msan -c clang -t Debug \
#     -f "-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer" \
#     -x "-DMSAN_LIBCXX_PATH=$HOME/msan-libcxx" \
#     -x "-DBUILD_BENCHMARKS=OFF" \
#     -x "-DWITH_JEMALLOC=OFF"

set -euo pipefail

INSTALL_DIR="${1:-${HOME}/msan-libcxx}"

# Detect clang version (e.g. "21" from "clang version 21.1.2")
CLANG_VERSION=$(clang --version | head -1 | grep -oP '\d+' | head -1)
if [ -z "$CLANG_VERSION" ]; then
  echo "ERROR: could not detect clang version" >&2
  exit 1
fi
echo "Detected clang major version: ${CLANG_VERSION}"

# Check for existing installation
if [ -f "${INSTALL_DIR}/lib/libc++.a" ]; then
  echo "MSan-instrumented libc++ already exists at ${INSTALL_DIR}"
  echo "Remove it first if you want to rebuild."
  exit 0
fi

BUILD_DIR=$(mktemp -d)
trap "echo 'Build dir: ${BUILD_DIR} (remove manually if no longer needed)'" EXIT

echo "=== Building MSan-instrumented libc++ ==="
echo "  LLVM version: ${CLANG_VERSION}"
echo "  Install dir:  ${INSTALL_DIR}"
echo "  Build dir:    ${BUILD_DIR}"
echo ""

# Clone just the runtimes from llvm-project.
# Use the release branch matching our clang version.
LLVM_BRANCH="release/${CLANG_VERSION}.x"
echo "Cloning llvm-project (branch ${LLVM_BRANCH}, sparse: runtimes only)..."

cd "${BUILD_DIR}"
git init llvm-project
cd llvm-project
git remote add origin https://github.com/llvm/llvm-project.git
git config core.sparseCheckout true
cat >.git/info/sparse-checkout <<'EOF'
/cmake
/runtimes
/libcxx
/libcxxabi
/libunwind
/llvm/cmake
/llvm/utils/llvm-lit
/third-party
EOF
git fetch --depth 1 origin "${LLVM_BRANCH}"
git checkout FETCH_HEAD
cd "${BUILD_DIR}"

echo ""
echo "Configuring..."

cmake -G "Unix Makefiles" \
  -S llvm-project/runtimes \
  -B build \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-fuse-ld=lld" \
  -DCMAKE_CXX_FLAGS="-fuse-ld=lld" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" \
  -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=lld" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
  -DLLVM_USE_SANITIZER=MemoryWithOrigins \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DLIBCXX_ENABLE_SHARED=OFF \
  -DLIBCXXABI_ENABLE_SHARED=OFF \
  -DLIBUNWIND_ENABLE_SHARED=OFF \
  -DLIBCXX_ENABLE_BENCHMARKS=OFF \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF \
  -DLIBCXXABI_INCLUDE_TESTS=OFF \
  -DLIBUNWIND_INCLUDE_TESTS=OFF

echo ""
echo "Building (this may take a few minutes)..."
make -j"$(nproc)" -C build

echo ""
echo "Installing to ${INSTALL_DIR}..."
make -C build install

echo ""
echo "=== Done ==="
echo "MSan-instrumented libc++ installed to: ${INSTALL_DIR}"
echo ""
echo "To build hogd with MSan:"
echo "  ./scripts/build.sh -d build/llvm_msan -c clang -t Debug \\"
echo "    -f \"-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer\" \\"
echo "    -x \"-DMSAN_LIBCXX_PATH=${INSTALL_DIR}\" \\"
echo "    -x \"-DBUILD_BENCHMARKS=OFF\" \\"
echo "    -x \"-DWITH_JEMALLOC=OFF\""
