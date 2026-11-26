#!/bin/bash
#
# Locations shared by the scripts in this directory. Each can be set in the
# environment; the toolchain scripts also take them as options.

# Aarno Labs' LLVM fork and the release hogd is built against.
: "${HOGD_PTSAN_REPO:=https://github.com/Aarno-Labs/PointerTagSanitizer}"
: "${HOGD_PTSAN_REF:=0.4.0}"

# Outside the hogd tree by default: the checkout is 3 GB and the build larger,
# and neither changes with the hogd commit.
: "${HOGD_PTSAN_HOME:=${HOME}/.cache/hogd-ptsan}"
: "${HOGD_PTSAN_SRC:=${HOGD_PTSAN_HOME}/PointerTagSanitizer}"
: "${HOGD_PTSAN_TOOLCHAIN:=${HOGD_PTSAN_HOME}/toolchain}"
