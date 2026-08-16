#!/usr/bin/env bash

# Selftest for scripts/ci/ctest_report.sh.
#
# Runs the wrapper against a fake ctest and hand-written Testing/Temporary logs,
# so the replay can be checked without building anything. Every case here is a
# property the sanitizer schedule depends on: the wrapper must be transparent
# when ctest passes, must replay exactly the failing tests when it does not,
# must forward ctest's exit status either way, and must not turn a missing log
# into a failure of its own.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CTEST_REPORT="$SCRIPT_DIR/ctest_report.sh"

checks_run=0
checks_failed=0

check() {
    local description=$1
    local expected=$2
    local actual=$3

    checks_run=$((checks_run + 1))
    if [ "$expected" = "$actual" ]; then
        echo "ok: $description"
        return
    fi
    checks_failed=$((checks_failed + 1))
    echo "FAIL: $description"
    echo "  expected: $expected"
    echo "  actual:   $actual"
}

check_contains() {
    local description=$1
    local haystack=$2
    local needle=$3

    checks_run=$((checks_run + 1))
    if [[ $haystack == *"$needle"* ]]; then
        echo "ok: $description"
        return
    fi
    checks_failed=$((checks_failed + 1))
    echo "FAIL: $description"
    echo "  expected to contain: $needle"
    echo "  in: $haystack"
}

check_not_contains() {
    local description=$1
    local haystack=$2
    local needle=$3

    checks_run=$((checks_run + 1))
    if [[ $haystack != *"$needle"* ]]; then
        echo "ok: $description"
        return
    fi
    checks_failed=$((checks_failed + 1))
    echo "FAIL: $description"
    echo "  expected NOT to contain: $needle"
    echo "  in: $haystack"
}

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# A fake ctest that exits with whatever FAKE_CTEST_STATUS says and records the
# arguments it was handed, so pass-through can be checked.
FAKE_BIN="$WORK_DIR/bin"
mkdir -p "$FAKE_BIN"
cat >"$FAKE_BIN/ctest" <<'FAKE_CTEST'
#!/usr/bin/env bash
printf '%s\n' "$@" >"$FAKE_CTEST_ARGS_FILE"
echo "fake ctest ran"
exit "${FAKE_CTEST_STATUS:-0}"
FAKE_CTEST
chmod +x "$FAKE_BIN/ctest"
export PATH="$FAKE_BIN:$PATH"
export FAKE_CTEST_ARGS_FILE="$WORK_DIR/ctest-args"

# Writes a Testing/Temporary pair under a build directory: the failing test
# names, and a LastTest.log holding one block per test in ctest's own format.
write_ctest_logs() {
    local build_dir=$1
    local failed_names=$2
    local temporary_dir="$build_dir/Testing/Temporary"

    mkdir -p "$temporary_dir"
    printf '%s' "$failed_names" >"$temporary_dir/LastTestsFailed.log"
    cat >"$temporary_dir/LastTest.log" <<'LAST_TEST'
Start testing: Aug 16 14:39 UTC
----------------------------------------------------------
1/3 Testing: passing_test
1/3 Test: passing_test
Command: "/usr/bin/true"
Directory: /build
"passing_test" start time: Aug 16 14:39 UTC
Output:
----------------------------------------------------------
output of the passing test
<end of output>
Test time =   0.01 sec
----------------------------------------------------------
Test Passed.
"passing_test" end time: Aug 16 14:39 UTC
"passing_test" time elapsed: 00:00:00
----------------------------------------------------------

2/3 Testing: rust_workspace_tests
2/3 Test: rust_workspace_tests
Command: "cargo" "test" "--workspace"
Directory: /build/hogd/rust
"rust_workspace_tests" start time: Aug 16 14:39 UTC
Output:
----------------------------------------------------------
running 31 tests
assertion failed: the interesting part
<end of output>
Test time =   3.20 sec
----------------------------------------------------------
Test Failed.
"rust_workspace_tests" end time: Aug 16 14:39 UTC
"rust_workspace_tests" time elapsed: 00:00:03
----------------------------------------------------------

3/3 Testing: threadpool
3/3 Test: threadpool
Command: "/build/threadpool"
Directory: /build
"threadpool" start time: Aug 16 14:39 UTC
Output:
----------------------------------------------------------
timed out waiting for a worker
<end of output>
Test time = 100.00 sec
----------------------------------------------------------
Test Timeout.
"threadpool" end time: Aug 16 14:41 UTC
"threadpool" time elapsed: 00:01:40
----------------------------------------------------------

End testing: Aug 16 14:41 UTC
LAST_TEST
}

echo "=== a passing ctest run is passed straight through"

BUILD_DIR="$WORK_DIR/build-pass"
mkdir -p "$BUILD_DIR"
export FAKE_CTEST_STATUS=0
output=$("$CTEST_REPORT" --test-dir "$BUILD_DIR" --parallel \
    --timeout 100 --output-on-failure 2>&1)
status=$?
check "passing run exits 0" "0" "$status"
check_contains "passing run keeps ctest's own output" "$output" "fake ctest ran"
check_not_contains "passing run replays nothing" "$output" "replaying"
check "arguments reach ctest unchanged" \
    "--test-dir
$BUILD_DIR
--parallel
--timeout
100
--output-on-failure" \
    "$(cat "$FAKE_CTEST_ARGS_FILE")"

echo "=== a failing run replays the failing tests and only those"

BUILD_DIR="$WORK_DIR/build-fail"
write_ctest_logs "$BUILD_DIR" '2:rust_workspace_tests
3:threadpool
'
export FAKE_CTEST_STATUS=8
output=$("$CTEST_REPORT" --test-dir "$BUILD_DIR" --parallel \
    --output-on-failure 2>&1)
status=$?
check "failing run forwards ctest's status" "8" "$status"
check_contains "failing test's output is replayed" "$output" \
    "assertion failed: the interesting part"
check_contains "a second failing test is replayed too" "$output" \
    "timed out waiting for a worker"
check_contains "each replay is labelled with its test" "$output" \
    "===== rust_workspace_tests: output captured by ctest ====="
check_not_contains "a passing test is not replayed" "$output" \
    "output of the passing test"
check_not_contains "the log trailer is not replayed" "$output" "End testing:"
check_not_contains "nothing is omitted under the default cap" "$output" \
    "line(s) of output omitted"

echo "=== --test-dir=<dir> is understood as well as --test-dir <dir>"

output=$("$CTEST_REPORT" "--test-dir=$BUILD_DIR" --parallel 2>&1)
check_contains "joined --test-dir finds the same logs" "$output" \
    "assertion failed: the interesting part"

echo "=== the replay is capped, and says what it dropped"

export CTEST_FAILURE_REPORT_LINES=6
output=$("$CTEST_REPORT" --test-dir "$BUILD_DIR" 2>&1)
unset CTEST_FAILURE_REPORT_LINES
check_contains "the tail is what survives the cap" "$output" "Test Failed."
check_not_contains "the head is dropped by the cap" "$output" \
    "assertion failed: the interesting part"
check_contains "the drop is stated, not silent" "$output" \
    "line(s) of output omitted"

echo "=== a run with no captured logs still reports ctest's failure"

BUILD_DIR="$WORK_DIR/build-nologs"
mkdir -p "$BUILD_DIR"
output=$("$CTEST_REPORT" --test-dir "$BUILD_DIR" 2>&1)
status=$?
check "missing logs do not change the status" "8" "$status"
check_contains "missing logs are reported" "$output" \
    "ctest_report: no captured output to replay"

echo
echo "$checks_run checks, $checks_failed failed"
[ "$checks_failed" -eq 0 ]
