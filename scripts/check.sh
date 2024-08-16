#!/bin/bash

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/../
BUILD_DIR=bcheck

COLOR='\033[1;34m'
NC='\033[0m'

printf "${COLOR}STEP 1: Check build (gcc)\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}_gcc -t Debug -c g++ -f "-Werror" ; then
    printf "build failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 2: Check tests (gcc)\n${NC}"
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}_gcc --output-on-failure; then
    printf "tests failed!\n"
    exit 1
fi

printf "${COLOR}STEP 3: Check build (llvm)\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}_llvm -t Debug -c clang++ -f "-Werror" ; then
    printf "build failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 4: Check tests (llvm)\n${NC}"
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}_llvm --output-on-failure; then
    printf "tests failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 5: Check linter\n${NC}"
if ! ${SCRIPT_DIR}/tidy.sh -d ${BUILD_DIR}_llvm; then
    printf "linter failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 6: Check format\n${NC}"
if ! ${SCRIPT_DIR}/format.sh -c; then
    printf "format failed!\n"
    exit 1
fi
printf "Format checking passed.\n"

printf "${COLOR}\nAll checks passed!\n${NC}"
