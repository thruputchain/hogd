#!/bin/bash

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh
BUILD_DIR=bcheck

print_usage() {
  printf "usage: scripts/check.sh [OPTION]...\n"
  printf -- "-e enable extra checks\n"
}

while getopts 'e' flag; do
  case "${flag}" in
    e) EXTRA=yes ;;
    *) print_usage
       exit 1 ;;
    esac
done

mkdir -p "$BUILD_DIR"

COLOR='\033[1;34m'
NC='\033[0m'

printf "${COLOR}STEP 1: Check build (gcc)\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/gcc -t Debug -c gcc -f "-Werror" ; then
    printf "build failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 2: Check tests (gcc)\n${NC}"
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/gcc --output-on-failure; then
    printf "tests failed!\n"
    exit 1
fi

printf "${COLOR}STEP 3: Check build (llvm)\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm -t Debug -c clang -f "-Werror" ; then
    printf "build failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 4: Check tests (llvm)\n${NC}"
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/llvm --output-on-failure; then
    printf "tests failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 5: Check linter\n${NC}"
if ! ${SCRIPT_DIR}/tidy.sh -d ${BUILD_DIR}/llvm; then
    printf "linter failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 6: Check format\n${NC}"
if ! ${SCRIPT_DIR}/format.sh -c; then
    printf "format failed!\n"
    exit 1
fi
printf "Format checking passed.\n"

if [ -z ${EXTRA+x} ]; then
    printf "${COLOR}\nAll checks passed!\n${NC}"
    exit 0
fi

printf "${COLOR}\nSTEP 7: Check tsan\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm_tsan -t Debug -c clang -f "-Werror -fsanitize=thread" ; then
    printf "tsan build failed!\n"
    exit 1
fi
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/llvm_tsan --output-on-failure; then
    printf "tsan tests failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 8: Check asan\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm_asan -t Debug -c clang -f "-Werror -fsanitize=address" ; then
    printf "asan build failed!\n"
    exit 1
fi
# unfortunately alloc_dealloc_mismatch is blocked on https://github.com/llvm/llvm-project/issues/52771
if ! ASAN_OPTIONS=alloc_dealloc_mismatch=0 ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/llvm_asan --output-on-failure; then
    printf "asan tests failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 9: Check ubsan\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm_ubsan -t Debug -c clang -f "-Werror -fsanitize=undefined" ; then
    printf "ubsan build failed!\n"
    exit 1
fi
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/llvm_ubsan --output-on-failure; then
    printf "ubsan tests failed!\n"
    exit 1
fi

printf "${COLOR}\nAll checks passed!\n${NC}"
