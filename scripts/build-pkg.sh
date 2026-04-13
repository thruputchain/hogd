#!/bin/bash

set -ex

WORK_DIR=$(pwd)
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
HOGD_ROOT=${SCRIPT_DIR}/..

print_usage() {
  printf "usage: scripts/build-pkg.sh [OPTION]...\n"
  printf -- "  -c sets the compiler (e.g., 'gcc', 'clang')\n"
  printf -- "  -d sets the build directory (default is 'build')\n"
  printf -- "  -j sets the number of build jobs\n"
  printf -- "  -t sets the build type (can be 'Release' or 'Debug')\n"
}

while getopts 'c:d:j:t:' flag; do
  case "${flag}" in
  c)
    COMPILER_FLAG="${OPTARG}"
    ;;
  d) BUILD_DIR_FLAG="${OPTARG}" ;;
  j) BUILD_JOBS_FLAG="${OPTARG}" ;;
  t)
    if [[ "${OPTARG}" == "Debug" || "${OPTARG}" == "Release" ]]; then
      BUILD_TYPE_FLAG="${OPTARG}"
    else
      print_usage
      exit 1
    fi
    ;;
  *)
    print_usage
    exit 1
    ;;
  esac
done

BUILD_DIR=./build-pkg
BUILD_TYPE="Release"

BUILD_ARGS=()
# BUILD_ARGS+=(-x -DX23_CORO_TRACE=1)

if [[ -n "${COMPILER_FLAG}" ]]; then
  BUILD_ARGS+=(-c "${COMPILER_FLAG}")
fi

if [[ -n "${BUILD_DIR_FLAG}" ]]; then
  BUILD_DIR="${BUILD_DIR_FLAG}"
fi
BUILD_DIR=$(realpath "${BUILD_DIR}")
mkdir -p ${BUILD_DIR}
BUILD_ARGS+=(-d "${BUILD_DIR}")

if [[ -n "${BUILD_TYPE_FLAG}" ]]; then
  BUILD_TYPE="${BUILD_TYPE_FLAG}"
fi
BUILD_ARGS+=(-t "${BUILD_TYPE}")

if [[ -n "${BUILD_JOBS_FLAG}" ]]; then
  BUILD_ARGS+=(-j "${BUILD_JOBS_FLAG}")
fi

echo "building in ${BUILD_DIR}"

# build

# source ${HOGD_ROOT}/scripts/ci/prepare_env.sh > prepare_env.log 2>&1 || (cat prepare_env.log && exit 1)

echo 'building hogd'
${SCRIPT_DIR}/build.sh "${BUILD_ARGS[@]}"

echo 'building chuckd & scpd'
# These build scripts place the binaries in the current working directory, so we
# [pushd BUILD_DIR] so that the binaries are placed in the [BUILD_DIR] directory.
pushd ${BUILD_DIR} >/dev/null
${SCRIPT_DIR}/../groundhog-go/chuckd/scripts/build_chuckd.sh >build_chuckd.log 2>&1 || (cat build_chuckd.log && exit 1)
${SCRIPT_DIR}/../groundhog-go/chuckd/scripts/build_scpd.sh >build_scpd.log 2>&1 || (cat build_scpd.log && exit 1)

echo 'building hogx'
${SCRIPT_DIR}/build-hogx.sh >build_hogx.log 2>&1 || (cat build_hogx.log && exit 1)
popd >/dev/null

# package

echo "packaging"

PKG_DIR=${BUILD_DIR}/pkg
mkdir -p ${PKG_DIR}

cp ${BUILD_DIR}/chuckd.dev ${PKG_DIR}/chuckd
cp ${BUILD_DIR}/scpd.dev ${PKG_DIR}/scpd
cp ${BUILD_DIR}/hogx-agent ${PKG_DIR}/hogx-agent
cp ${BUILD_DIR}/hogx ${PKG_DIR}/hogx

cp ${BUILD_DIR}/hogd/hogdb/hogdb ${PKG_DIR}/hogdb
cp ${BUILD_DIR}/hogd/hogexec/hogexec ${PKG_DIR}/hogexec
cp ${BUILD_DIR}/hogd/event/event_exec ${PKG_DIR}/event_exec
cp ${BUILD_DIR}/hogd/indexer/indexer_exec ${PKG_DIR}/indexer_exec
cp ${BUILD_DIR}/hogd/mempool/mempool_exec ${PKG_DIR}/mempool_exec
cp ${BUILD_DIR}/hogd/auth/auth_exec ${PKG_DIR}/auth_exec
cp ${BUILD_DIR}/hogd/loadgen/loadgen_server ${PKG_DIR}/loadgen_server

# compute package name

cd ${SCRIPT_DIR}
GIT_COMMIT=$(git rev-parse HEAD | cut -c1-6)

ARCH=$(uname -m)
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
BUILD_TAG=$(echo ${BUILD_TYPE} | tr '[:upper:]' '[:lower:]')

PKG_NAME=hog-${ARCH}-${OS}-${BUILD_TAG}-${GIT_COMMIT}
echo ${PKG_NAME} >${PKG_DIR}/PKG_ID

tar -czvf ${BUILD_DIR}/${PKG_NAME}.tar.gz -C ${PKG_DIR} .

echo "created ${BUILD_DIR}/${PKG_NAME}.tar.gz"
