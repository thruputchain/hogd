#!/bin/bash

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh
BUILD_DIR=bcheck
PARALLEL=
BUILD_PARALLEL=no

print_usage() {
  printf "usage: scripts/check.sh [OPTION]...\n"
  printf -- "-e enable extra checks\n"
  printf -- "-p run tests in parallel\n"
  printf -- "-b do builds in parallel\n"
  printf -- "-d <dir> specify build directory (default: $BUILD_DIR)\n"
}

while getopts 'epbd:' flag; do
  case "${flag}" in
  e) EXTRA=yes ;;
  p) PARALLEL=--parallel ;;
  b) BUILD_PARALLEL=yes ;;
  d) BUILD_DIR="$OPTARG" ;;
  *)
    print_usage
    exit 1
    ;;
  esac
done

mkdir -p "$BUILD_DIR"

COLOR='\033[1;34m'
NC='\033[0m'

GCC_ERRORS_LOG=${SCRIPT_DIR}/gcc_errors.log
GCC_RELEASE_ERRORS_LOG=${SCRIPT_DIR}/gcc_release_errors.log
LLVM_ERRORS_LOG=${SCRIPT_DIR}/llvm_errors.log

# Optionally perform the second build in parallel, in the background;
# Much of the CI time is bottlenecked on libsodium's configure step (sequential, low CPU usage)
# and a few late-stage link jobs
if [ "$BUILD_PARALLEL" = "yes" ]; then
  LLVM_LOG=${BUILD_DIR}/llvm_build.log
  GCC_RELEASE_LOG=${BUILD_DIR}/gcc_release_build.log
  ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm -t Debug -c clang -f "-Werror" -x "-DENABLE_CI_FAIL_LOG=${LLVM_ERRORS_LOG}" >"$LLVM_LOG" 2>&1 &
  LLVM_BUILD_PID=$!
  ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/gcc_release -t Release -c gcc -x "-DENABLE_CI_FAIL_LOG=${GCC_RELEASE_ERRORS_LOG}" >"$GCC_RELEASE_LOG" 2>&1 &
  GCC_RELEASE_BUILD_PID=$!
fi

GCC_DEBUG_LOG=${BUILD_DIR}/gcc_debug_build.log
if [ "$BUILD_PARALLEL" = "yes" ]; then
  ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/gcc -t Debug -c gcc -f "-Werror" -x "-DENABLE_CI_FAIL_LOG=${GCC_ERRORS_LOG}" >"$GCC_DEBUG_LOG" 2>&1 &
  GCC_DEBUG_BUILD_PID=$!
fi

printf "${COLOR}\nSTEP 1: Check format\n${NC}"
if ! ${SCRIPT_DIR}/format.sh -c; then
  printf "format failed!\n"
  exit 1
fi
printf "Format checking passed.\n"

if [ "$BUILD_PARALLEL" = "yes" ]; then
  printf "${COLOR}Waiting for Step 2 (gcc debug) build to finish...\n${NC}"
  wait $GCC_DEBUG_BUILD_PID
  GCC_DEBUG_STATUS=$?
  if [ $GCC_DEBUG_STATUS -ne 0 ]; then
    printf "gcc debug build failed!\n"
    printf "\n===== gcc debug build log =====\n"
    cat "$GCC_DEBUG_LOG"
    printf "===== end gcc debug build log =====\n\n"
    printf "\n===== gcc debug errors =====\n"
    cat "${GCC_ERRORS_LOG}"
    printf "===== end gcc debug errors =====\n\n"
    exit 1
  fi
else
  printf "${COLOR}STEP 2: Check build (gcc)\n${NC}"
  if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/gcc -t Debug -c gcc -f "-Werror" -x "-DENABLE_CI_FAIL_LOG=${GCC_ERRORS_LOG}"; then
    printf "build failed!\n"
    cat "${GCC_ERRORS_LOG}"
    exit 1
  fi
fi

printf "${COLOR}\nSTEP 3: Check tests (gcc)\n${NC}"
if ! ctest --test-dir ${BUILD_DIR}/gcc --output-on-failure ${PARALLEL} --timeout 1000000; then
  printf "tests failed!\n"
  exit 1
fi

if [ "$BUILD_PARALLEL" = "yes" ]; then
  printf "${COLOR}Waiting for Step 4 (llvm) build to finish...\n${NC}"
  wait $LLVM_BUILD_PID
  LLVM_STATUS=$?
  if [ $LLVM_STATUS -ne 0 ]; then
    printf "llvm build failed!\n"
    printf "\n===== llvm build log =====\n"
    cat "$LLVM_LOG"
    printf "===== end llvm build log =====\n\n"
    printf "\n===== llvm errors =====\n"
    cat "${LLVM_ERRORS_LOG}"
    printf "===== end llvm errors =====\n\n"
    exit 1
  fi
  printf "${COLOR}Step 4 (llvm) build finished\n${NC}"

  printf "${COLOR}STEP 6: Check linter\n${NC}"
  TIDY_LOG=${BUILD_DIR}/tidy.log
  ${SCRIPT_DIR}/tidy.sh -d ${BUILD_DIR}/llvm >"$TIDY_LOG" 2>&1 &
  TIDY_PID=$!
else
  printf "${COLOR}STEP 3: Check build (llvm)\n${NC}"
  if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm -t Debug -c clang -f "-Werror" -x "-DENABLE_CI_FAIL_LOG=${LLVM_ERRORS_LOG}"; then
    printf "build failed!\n"
    printf "\n===== llvm errors =====\n"
    cat "${BUILD_DIR}/llvm_errors.log"
    printf "===== end llvm errors =====\n\n"
    exit 1
  fi
fi

printf "${COLOR}\nSTEP 5: Check tests (llvm)\n${NC}"
if ! ctest --test-dir ${BUILD_DIR}/llvm --output-on-failure ${PARALLEL} --timeout 1000000; then
  printf "tests failed!\n"
  exit 1
fi

if [ $BUILD_PARALLEL = "yes" ]; then
  wait $TIDY_PID
  TIDY_STATUS=$?
  if [ $TIDY_STATUS -ne 0 ]; then
    printf "tidy failed!\n"
    printf "\n===== tidy log =====\n"
    cat "$TIDY_LOG"
    printf "===== end tidy log =====\n\n"
    exit 1
  fi
  printf "${COLOR}Step 6 (tidy) finished\n${NC}"
else
  printf "${COLOR}\nSTEP 6: Check linter\n${NC}"
  if ! ${SCRIPT_DIR}/tidy.sh -d ${BUILD_DIR}/llvm; then
    printf "linter failed!\n"
    exit 1
  fi
fi

if [ "$BUILD_PARALLEL" = "yes" ]; then
  printf "${COLOR}Waiting for Step 7 (gcc_release) build to finish...\n${NC}"
  wait $GCC_RELEASE_BUILD_PID
  GCC_RELEASE_STATUS=$?
  if [ $GCC_RELEASE_STATUS -ne 0 ]; then
    printf "gcc_release build failed!\n"
    printf "\n===== gcc_release build log =====\n"
    cat "$GCC_RELEASE_LOG"
    printf "===== end gcc_release build log =====\n\n"
    printf "\n===== gcc_release errors =====\n"
    cat "${GCC_RELEASE_ERRORS_LOG}"
    printf "===== end gcc_release errors =====\n\n"
    exit 1
  fi
  printf "${COLOR}Step 7 (gcc_release) build finished\n${NC}"
else
  printf "${COLOR}\nSTEP 7: Check release build (gcc)\n${NC}"
  if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/gcc_release -t Release -c gcc -x "-DENABLE_CI_FAIL_LOG=${GCC_RELEASE_ERRORS_LOG}"; then
    printf "build failed!\n"
    printf "\n===== gcc_release errors =====\n"
    cat "${GCC_RELEASE_ERRORS_LOG}"
    printf "===== end gcc_release errors =====\n\n"
    exit 1
  fi
fi

if [ -z ${EXTRA+x} ]; then
  printf "${COLOR}\nAll checks passed!\n${NC}"
  exit 0
fi

printf "${COLOR}\nSTEP 8: Check tsan\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm_tsan -t Debug -c clang -f "-Werror -fsanitize=thread"; then
  printf "tsan build failed!\n"
  exit 1
fi
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/llvm_tsan --output-on-failure; then
  printf "tsan tests failed!\n"
  exit 1
fi

printf "${COLOR}\nSTEP 9: Check asan\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm_asan -t Debug -c clang -f "-Werror -fsanitize=address"; then
  printf "asan build failed!\n"
  exit 1
fi
# unfortunately alloc_dealloc_mismatch is blocked on https://github.com/llvm/llvm-project/issues/52771
if ! ASAN_OPTIONS=alloc_dealloc_mismatch=0 ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/llvm_asan --output-on-failure; then
  printf "asan tests failed!\n"
  exit 1
fi

printf "${COLOR}\nSTEP 10: Check ubsan\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm_ubsan -t Debug -c clang -f "-Werror -fsanitize=undefined"; then
  printf "ubsan build failed!\n"
  exit 1
fi
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/llvm_ubsan --output-on-failure; then
  printf "ubsan tests failed!\n"
  exit 1
fi

printf "${COLOR}\nAll checks passed!\n${NC}"
