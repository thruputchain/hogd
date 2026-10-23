#!/usr/bin/env bash
# Self-test for fetch_go_modules: a download the network dropped must be
# retried, a download that keeps failing must come back with go's own status
# once the attempts run out, and a tree with no modules in it must still be
# reported rather than silently prefetching nothing.
set -euo pipefail

SCRIPT="$(dirname "$(readlink -f "$0")")/fetch_go_modules.sh"
fail=0

expect_status() {
  local label=$1 want=$2 got=$3
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: exit status $got, want $want"
    fail=1
  else
    echo "ok   $label status"
  fi
}

expect_calls() {
  local label=$1 want=$2 log=$3 got
  got=$(wc -l <"$log")
  if [[ "$got" -ne "$want" ]]; then
    echo "FAIL $label: go ran $got times, want $want"
    sed 's/^/     | /' "$log"
    fail=1
  else
    echo "ok   $label call count"
  fi
}

expect_contains() {
  local label=$1 needle=$2 haystack=$3
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "FAIL $label: output missing [$needle]"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  else
    echo "ok   $label"
  fi
}

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# A go whose behaviour per call comes from GO_FAKE_SCRIPT: one word per call,
# [ok] or an exit status to report. Every call records the module directory it
# ran in, so a test can tell one module from another and an attempt from a
# retry.
mkdir -p "$work/bin"
cat >"$work/bin/go" <<'FAKE'
#!/usr/bin/env bash
pwd >> "$GO_FAKE_LOG"
read -r -a behaviours <<< "$GO_FAKE_SCRIPT"
call=$(wc -l < "$GO_FAKE_LOG")
behaviour=${behaviours[$((call - 1))]:-${behaviours[-1]}}
case "$behaviour" in
  ok) exit 0 ;;
  *)
    echo "go: proxy.golang.org: read: connection reset by peer" >&2
    exit "$behaviour"
    ;;
esac
FAKE
chmod +x "$work/bin/go"
export PATH="$work/bin:$PATH"

# A copy of the script under a fake repository root, so the tree it walks is
# this test's rather than the checkout's. The script finds that root relative
# to its own path, so the copy has to keep the scripts/ci/ layout.
new_tree() {
  local tree=$1
  shift
  mkdir -p "$tree/scripts/ci"
  cp "$SCRIPT" "$tree/scripts/ci/fetch_go_modules.sh"
  local module
  for module in "$@"; do
    mkdir -p "$tree/groundhog-go/$module"
    echo "module example.com/$(basename "$module")" >"$tree/groundhog-go/$module/go.mod"
  done
}

# Retries cost the test nothing: the point is that they happen, not how long
# the script waits between them.
export FETCH_GO_MODULES_RETRY_DELAY_SEC=0

# Any further arguments are NAME=VALUE overrides for that one run.
run_tree() {
  local tree=$1 behaviours=$2
  shift 2
  : >"$work/calls"
  env GO_FAKE_LOG="$work/calls" GO_FAKE_SCRIPT="$behaviours" "$@" \
    bash "$tree/scripts/ci/fetch_go_modules.sh" 2>&1
}

# A download that works is left alone.
new_tree "$work/clean" chuckd
status=0
output=$(run_tree "$work/clean" "ok") || status=$?
expect_status "first-try download" 0 "$status"
expect_calls "first-try download" 1 "$work/calls"

# The failure from the issue: the proxy drops the connection partway through.
# The next attempt resumes and the host script carries on.
new_tree "$work/blip" chuckd
status=0
output=$(run_tree "$work/blip" "1 ok") || status=$?
expect_status "retried download" 0 "$status"
expect_calls "retried download" 2 "$work/calls"
expect_contains "retried download reports the retry" "retrying in 0s (attempt 1/3)" "$output"

# A proxy that is really down is not retried forever, and the caller gets go's
# status rather than one this script made up.
new_tree "$work/down" chuckd
status=0
output=$(run_tree "$work/down" "3") || status=$?
expect_status "exhausted attempts" 3 "$status"
expect_calls "exhausted attempts" 3 "$work/calls"
expect_contains "exhausted attempts report the last failure" "after 3 attempt(s)" "$output"
expect_contains "exhausted attempts keep go's stderr" "connection reset by peer" "$output"

# Attempts are per module: one module's blip must not spend another's budget,
# and every module in the tree still gets downloaded.
new_tree "$work/many" chuckd lib-go protocol-go
status=0
output=$(run_tree "$work/many" "1 ok ok ok") || status=$?
expect_status "several modules" 0 "$status"
expect_calls "several modules" 4 "$work/calls"

# vendor/ holds a module's dependencies, not a module to prefetch for.
new_tree "$work/vendored" chuckd chuckd/vendor/example.com/dep
status=0
output=$(run_tree "$work/vendored" "ok") || status=$?
expect_status "vendored module skipped" 0 "$status"
expect_calls "vendored module skipped" 1 "$work/calls"

# An empty submodule means the checkout went wrong; prefetching nothing and
# reporting success would push that failure into the build.
new_tree "$work/empty"
mkdir -p "$work/empty/groundhog-go"
status=0
output=$(run_tree "$work/empty" "ok") || status=$?
expect_status "no modules" 1 "$status"
expect_calls "no modules" 0 "$work/calls"
expect_contains "no modules reported" "No Go modules found in groundhog-go" "$output"

# A zero-padded override is the decimal an operator wrote, not an octal
# literal. Reading it as one left the attempt comparison erroring rather than
# comparing, and the download retried without end -- the stalled job this
# retry is supposed to prevent.
new_tree "$work/padded" chuckd
status=0
output=$(run_tree "$work/padded" "1 1 1 1 1 1 1 ok" FETCH_GO_MODULES_ATTEMPTS=08) || status=$?
expect_status "zero-padded attempt limit" 0 "$status"
expect_calls "zero-padded attempt limit" 8 "$work/calls"

new_tree "$work/padded-delay" chuckd
status=0
output=$(run_tree "$work/padded-delay" "1 ok" FETCH_GO_MODULES_RETRY_DELAY_SEC=00) || status=$?
expect_status "zero-padded delay" 0 "$status"
expect_calls "zero-padded delay" 2 "$work/calls"

# A typo must stop the script where it can still be read, rather than
# evaluating to zero and leaving a prefetch that never retries.
new_tree "$work/typo" chuckd
status=0
output=$(run_tree "$work/typo" "ok" FETCH_GO_MODULES_ATTEMPTS=three) || status=$?
expect_status "non-numeric attempt limit" 1 "$status"
expect_calls "non-numeric attempt limit" 0 "$work/calls"
expect_contains "non-numeric attempt limit reported" \
  "FETCH_GO_MODULES_ATTEMPTS must be a decimal integer" "$output"

# Zero attempts is not a prefetch with the retry off, it is no prefetch.
new_tree "$work/zero" chuckd
status=0
output=$(run_tree "$work/zero" "ok" FETCH_GO_MODULES_ATTEMPTS=0) || status=$?
expect_status "zero attempt limit" 1 "$status"
expect_calls "zero attempt limit" 0 "$work/calls"
expect_contains "zero attempt limit reported" \
  "FETCH_GO_MODULES_ATTEMPTS must be at least 1" "$output"

if [[ "$fail" -ne 0 ]]; then
  echo "fetch_go_modules selftest FAILED"
  exit 1
fi
echo "fetch_go_modules selftest passed"
