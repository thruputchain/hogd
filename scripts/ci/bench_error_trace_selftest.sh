#!/usr/bin/env bash
# Self-test for the ERR trap in bench_harness.sh: it must name the command that
# errexit kills a benchmark script with, and stay quiet otherwise.
set -euo pipefail

HARNESS="$(dirname "$(readlink -f "$0")")/bench_harness.sh"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT
fail=0

# Run a script body with the harness sourced, printing its combined output.
run_case() {
  printf '#!/usr/bin/env bash\nset -Eeuo pipefail\n. %q\n%s\n' "$HARNESS" "$1" >"$WORK_DIR/case.sh"
  CI_BENCH_FAILURE_LOG="$WORK_DIR/failures.log" bash "$WORK_DIR/case.sh" 2>&1 || true
}

check() {
  local label=$1 want=$2 got=$3
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: got [$got] want [$want]"
    fail=1
  else
    echo "ok   $label"
  fi
}

# The aborting command is named, on stderr and in the digest, and the script
# stops there.
rm -f "$WORK_DIR"/failures.log
out=$(run_case 'launch() { return 7; }
launch
echo "NOT REACHED"')
check 'reports the aborting command' 1 "$(grep -c 'exit status 7: return 7' <<<"$out")"
check 'stops at the failure' 0 "$(grep -c 'NOT REACHED' <<<"$out")"
check 'digests the aborting command' 1 "$(grep -c 'exit status 7: return 7' "$WORK_DIR/failures.log")"

# A function failing in a pipeline reports from its own subshell, and the parent
# shell then reports the pipeline itself. Only the first, specific report stands.
rm -f "$WORK_DIR"/failures.log
out=$(run_case 'produce() { /bin/false; }
produce | cat')
check 'pipeline reports once' 1 "$(grep -c '^FAILED ' <<<"$out")"
check 'pipeline names the failing command' 1 "$(grep -c ': /bin/false$' <<<"$out")"

# A command that dies inside a helper is reported at the helper's line, which
# every call site shares. The call stack must carry the frame back in the suite
# script, so the ~30 readiness waits that all go through [ci_wait_port] are told
# apart by their call site.
rm -f "$WORK_DIR"/failures.log
out=$(run_case 'wait_for_it() { /bin/false; }
: "line 5 is the next one"
wait_for_it')
check 'names the failing command' 1 "$(grep -c ': /bin/false$' <<<"$out")"
check 'stack names the call site in the script' 1 \
  "$(grep -c '^  in wait_for_it at .*/case\.sh:6$' <<<"$out")"
check 'digest carries the stack' 1 \
  "$(grep -c '^  in wait_for_it at .*/case\.sh:6$' "$WORK_DIR/failures.log")"
check 'stack does not add a second report' 1 "$(grep -c '^FAILED ' <<<"$out")"

# Nested helpers report every frame, outermost last, so a wait reached through
# ci_wait_port_hosts is distinguishable from a direct one.
rm -f "$WORK_DIR"/failures.log
out=$(run_case 'inner() { /bin/false; }
outer() { inner; }
outer')
check 'nested stack reports both frames' 2 "$(grep -c '^  in ' <<<"$out")"
check 'nested stack names the inner frame' 1 "$(grep -c '^  in inner at ' <<<"$out")"
check 'nested stack ends at the script call site' 1 \
  "$(grep -c '^  in outer at .*/case\.sh:6$' <<<"$out")"

# A failure at the top level of the script has no frames to walk, and still
# reports exactly one line.
rm -f "$WORK_DIR"/failures.log
out=$(run_case '/bin/false')
check 'top-level failure reports one line' 1 "$(grep -c '^FAILED ' <<<"$out")"
check 'top-level failure has no stack' 0 "$(grep -c '^  in ' <<<"$out")"

# The trap fires whether or not errexit is set, but a suite that tolerates a
# failure -- mempool_bench.sh takes the mempool's timeout as its expected end,
# cleanup handlers run after ci_enter_cleanup -- is not aborting.
rm -f "$WORK_DIR"/failures.log
out=$(run_case 'cleanup() { ci_enter_cleanup; false; }
trap cleanup EXIT
set +e
(exit 124)
set -e
false || true
echo "reached the end"')
check 'tolerated failures run on' 1 "$(grep -c 'reached the end' <<<"$out")"
check 'tolerated failures are not reported' 0 "$(grep -c '^FAILED ' <<<"$out")"
check 'tolerated failures leave no digest' 0 "$(cat "$WORK_DIR/failures.log" 2>/dev/null | wc -l)"

exit "$fail"
