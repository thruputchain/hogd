#!/usr/bin/env bash

# Runs ctest and, when it fails, replays the failing tests' captured output at
# the very end of the job's log.
#
# `--output-on-failure` already prints that output -- but it prints it at the
# moment the test finishes, and that is not where it can be read from. A
# `ctest --parallel` run over this repo's suite emits two progress lines per
# test, about six thousand lines, and a hog_sched job runs ctest up to three
# times. GitHub only serves the tail of a job log (roughly the last five
# thousand lines / 800KB through the API, and `gh run view --log` reads the
# same endpoint), so a test that fails early in the first ctest invocation has
# its output pushed past the end of what anyone can retrieve. What survives is
#
#     The following tests FAILED:
#             311 - rust_workspace_tests (Failed)
#
# and nothing else -- the name of the test, with no assertion, no stack, and no
# sanitizer report. That is what issue #3437 had to be diagnosed from.
#
# ctest writes every test's output to Testing/Temporary/LastTest.log and the
# names of the failing ones to Testing/Temporary/LastTestsFailed.log, both under
# the build directory, so the output is still on disk after the run. Replaying
# the failing tests' blocks here puts them in the last hundred lines of the job,
# which is always retrievable.
#
# Usage: drop-in for ctest, same arguments, same exit status:
#
#     ./scripts/ci/ctest_report.sh --test-dir build/gcc_asan --parallel \
#         --timeout 100 --output-on-failure
#
# Per test, the last CTEST_FAILURE_REPORT_LINES lines are replayed (the tail,
# because assertion messages and sanitizer reports land at the end of a dying
# test's output). The cap is what keeps the replay itself from overrunning the
# window it exists to fit inside.

set -uo pipefail

CTEST_FAILURE_REPORT_LINES=${CTEST_FAILURE_REPORT_LINES:-400}

# Finds the build directory ctest was pointed at, so we know where it left its
# logs. ctest resolves --test-dir the same way: last occurrence wins, and
# without one it tests the current directory.
ctest_test_dir() {
    local test_dir="."
    local expecting_value=0
    local arg

    for arg in "$@"; do
        if [ "$expecting_value" -eq 1 ]; then
            test_dir=$arg
            expecting_value=0
            continue
        fi
        case "$arg" in
            --test-dir)
                expecting_value=1
                ;;
            --test-dir=*)
                test_dir=${arg#--test-dir=}
                ;;
        esac
    done

    printf '%s\n' "$test_dir"
}

# Replays one block per failing test. Best effort: a missing or unreadable log
# must report that and leave ctest's own status alone -- this runs on a path
# that has already failed, and must not become the reason the failure is
# misread.
replay_ctest_failures() {
    local test_dir=$1
    local temporary_dir="$test_dir/Testing/Temporary"
    local failed_log="$temporary_dir/LastTestsFailed.log"
    local output_log="$temporary_dir/LastTest.log"

    if [ ! -r "$failed_log" ] || [ ! -r "$output_log" ]; then
        echo "ctest_report: no captured output to replay" \
            "($failed_log / $output_log unreadable)" >&2
        return
    fi

    awk -v tail_lines="$CTEST_FAILURE_REPORT_LINES" '
        # LastTestsFailed.log holds one "<number>:<name>" line per failing test.
        NR == FNR {
            name = $0
            sub(/^[0-9]+:/, "", name)
            if (name != "") {
                failed[name] = 1
            }
            next
        }

        # LastTest.log holds one block per test, opened by a "<i>/<n> Testing:
        # <name>" header. A block runs to the next header, so the header both
        # ends the previous block and starts the next.
        /^[0-9]+\/[0-9]+ Testing: / {
            flush_block()
            current = $0
            sub(/^[0-9]+\/[0-9]+ Testing: /, "", current)
            replaying = (current in failed)
            next
        }

        # The trailer closes the last block the same way a header would.
        /^End testing: / {
            flush_block()
            next
        }

        replaying {
            # Keep only the last tail_lines lines: an assertion message or a
            # sanitizer report is at the end of what a failing test printed.
            buffer[++held] = $0
            if (held - dropped > tail_lines) {
                delete buffer[++dropped]
            }
        }

        END {
            flush_block()
        }

        function flush_block(   i) {
            if (!replaying) {
                reset_block()
                return
            }
            printf "===== %s: output captured by ctest =====\n", current
            if (dropped > 0) {
                printf "[earlier %d line(s) of output omitted;" \
                    " raise CTEST_FAILURE_REPORT_LINES to see them]\n", dropped
            }
            for (i = dropped + 1; i <= held; i++) {
                print buffer[i]
            }
            printf "===== end of %s =====\n", current
            reset_block()
        }

        function reset_block(   i) {
            for (i in buffer) {
                delete buffer[i]
            }
            held = 0
            dropped = 0
            replaying = 0
        }
    ' "$failed_log" "$output_log"
}

main() {
    local test_dir
    local status=0

    ctest "$@" || status=$?
    if [ "$status" -eq 0 ]; then
        return 0
    fi

    test_dir=$(ctest_test_dir "$@")
    echo
    echo "===== ctest failed (status $status);" \
        "replaying the failing tests' output ====="
    echo "(repeated here because the copy printed during the run is past the" \
        "end of the retrievable job log)"
    replay_ctest_failures "$test_dir"

    return "$status"
}

main "$@"
