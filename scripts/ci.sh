#!/bin/bash

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
source ${SCRIPT_DIR}/env.sh
BUILD_DIR=bcheck

mkdir -p "$BUILD_DIR"

COLOR='\033[1;34m'
NC='\033[0m'

GCC_BUILD_AND_TEST_LOG=${BUILD_DIR}/gcc_build_and_test.log
LLVM_BUILD_AND_TEST_LOG=${BUILD_DIR}/llvm_build_and_test.log
GCC_RELEASE_BUILD_LOG=${BUILD_DIR}/gcc_release_build.log

(
printf "${COLOR}STEP 1: Check build (gcc debug)\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/gcc -t Debug -c gcc -f "-Werror" ; then
    printf "build failed!\n"
    exit 1
fi

printf "${COLOR}\nSTEP 2: Check tests (gcc debug)\n${NC}"
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/gcc --output-on-failure --timeout 100; then
    printf "tests failed!\n"
    exit 1
fi
) > ${GCC_BUILD_AND_TEST_LOG} 2>&1 &
GCC_PID=$!

(
printf "${COLOR}STEP 3: Check build (llvm debug)\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/llvm -t Debug -c clang -f "-Werror" ; then
    printf "build failed!\n"
    exit 1
fi

# Run tests and linter in parallel
(
printf "${COLOR}\nSTEP 4: Check tests (llvm debug)\n${NC}"
if ! ctest --test-dir ${ROOT_DIR}/${BUILD_DIR}/llvm --output-on-failure --timeout 100; then
    printf "tests failed!\n"
    exit 1
fi
) > ${BUILD_DIR}/llvm_test.log 2>&1 &
TEST_PID=$!

(
printf "${COLOR}STEP 5: Check linter\n${NC}"
if ! ${SCRIPT_DIR}/tidy.sh -d ${BUILD_DIR}/llvm; then
    printf "linter failed!\n"
    exit 1
fi
) > ${BUILD_DIR}/llvm_lint.log 2>&1 &
LINT_PID=$!

# Wait for both parallel processes to complete
wait $TEST_PID
TEST_EXIT_CODE=$?

wait $LINT_PID
LINT_EXIT_CODE=$?

# Print logs if either failed
if [ $TEST_EXIT_CODE -ne 0 ]; then
    printf "${COLOR}Tests failed with exit code $TEST_EXIT_CODE\n${NC}"
    printf "${COLOR}Begin test log:\n${NC}"
    cat ${BUILD_DIR}/llvm_test.log
    printf "${COLOR}End test log\n${NC}"
    exit $TEST_EXIT_CODE
fi

if [ $LINT_EXIT_CODE -ne 0 ]; then
    printf "${COLOR}Linter failed with exit code $LINT_EXIT_CODE\n${NC}"
    printf "${COLOR}Begin linter log:\n${NC}"
    cat ${BUILD_DIR}/llvm_lint.log
    printf "${COLOR}End linter log\n${NC}"
    exit $LINT_EXIT_CODE
fi

) > ${LLVM_BUILD_AND_TEST_LOG} 2>&1 &
LLVM_PID=$!

(
printf "${COLOR}STEP 7: Check build (gcc release)\n${NC}"
if ! ${SCRIPT_DIR}/build.sh -d ${BUILD_DIR}/gcc_release -t Release -c gcc ; then
    printf "build failed!\n"
    exit 1
fi

) > ${GCC_RELEASE_BUILD_LOG} 2>&1 &
GCC_RELEASE_PID=$!

printf "${COLOR}\nSTEP 6: Check format\n${NC}"
if ! ${SCRIPT_DIR}/format.sh -c; then
    printf "format failed!\n"
    exit 1
fi

wait $GCC_PID
GCC_EXIT_CODE=$?

if [ $GCC_EXIT_CODE -ne 0 ]; then
    printf "${COLOR}GCC build and test failed with exit code $GCC_EXIT_CODE\n${NC}"
    printf "${COLOR}Begin GCC build and test log:\n${NC}"
    cat ${GCC_BUILD_AND_TEST_LOG}
    printf "${COLOR}End GCC build and test log\n${NC}"
    exit $GCC_EXIT_CODE
fi 

wait $LLVM_PID
LLVM_EXIT_CODE=$?

if [ $LLVM_EXIT_CODE -ne 0 ]; then
    printf "${COLOR}LLVM build and test and tidy failed with exit code $LLVM_EXIT_CODE\n${NC}"
    printf "${COLOR}Begin LLVM build and test and tidy log:\n${NC}"
    cat ${LLVM_BUILD_AND_TEST_LOG}
    printf "${COLOR}End LLVM build and test and tidy log\n${NC}"
    exit $LLVM_EXIT_CODE
fi 

wait $GCC_RELEASE_PID
GCC_RELEASE_EXIT_CODE=$?

if [ $GCC_RELEASE_EXIT_CODE -ne 0 ]; then
    printf "${COLOR}GCC release build failed with exit code $GCC_RELEASE_EXIT_CODE\n${NC}"
    printf "${COLOR}Begin GCC release build log:\n${NC}"
    cat ${GCC_RELEASE_BUILD_LOG}
    printf "${COLOR}End GCC release build log\n${NC}"
    exit $GCC_RELEASE_EXIT_CODE
fi

printf "${COLOR}All checks passed!\n${NC}"
exit 0