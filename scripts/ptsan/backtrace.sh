#!/bin/bash
#
# Runs a PTSan-built program under gdb and prints a backtrace at the point
# where it fails; see docs/ptsan.md.
#
#   scripts/ptsan/backtrace.sh build/ptsan/hogd/base/base_test --gtest_filter='Rc.*'
#
# A PTSan report names a check by number and a function by hash, then exits
# without unwinding, so the report alone does not locate the access. The
# backtrace does: the frame above ptsan_complain is the checked access. gdb
# also stops on the SIGSEGV that a tagged pointer raises in code PTSan did not
# instrument.

set -euo pipefail

if (($# == 0)); then
  sed -n '3,6p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 1
fi

exec gdb -q -batch \
  -ex 'set pagination off' \
  -ex 'set breakpoint pending on' \
  -ex 'break ptsan_complain' \
  -ex 'break ptsan_complain_vector' \
  -ex 'run' \
  -ex 'backtrace 40' \
  --args "$@"
