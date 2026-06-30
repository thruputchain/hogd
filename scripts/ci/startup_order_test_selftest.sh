#!/usr/bin/env bash
# Self-test for the scenario -> launch-order mapping in startup_order_test.sh.
# Runs the script in list-only mode (no processes launched) and checks the
# printed order against expectations.
set -euo pipefail

SCRIPT="$(dirname "$(readlink -f "$0")")/startup_order_test.sh"
fail=0

check() {
  local scenario="$1" expected="$2" got
  got="$(HOG_STARTUP_LIST_ONLY=1 "$SCRIPT" "$scenario" | tr '\n' ' ' | sed 's/ $//')"
  if [[ "$got" != "$expected" ]]; then
    echo "FAIL $scenario: got [$got] want [$expected]"
    fail=1
  else
    echo "ok   $scenario"
  fi
}

# Every scenario must launch all 8 components exactly once.
check_complete() {
  local scenario="$1" got
  got="$(HOG_STARTUP_LIST_ONLY=1 "$SCRIPT" "$scenario" | sort | tr '\n' ' ' | sed 's/ $//')"
  local want="auth chuckd event hogdb hogexec indexer loadgen mempool"
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $scenario component set: got [$got] want [$want]"
    fail=1
  fi
}

check mempool-first "mempool loadgen hogexec event indexer hogdb auth chuckd"
check mempool-last "loadgen hogexec event indexer hogdb auth mempool chuckd"
check mempool-middle "loadgen hogexec event mempool indexer hogdb auth chuckd"

# Ordering relations for the indexer/event scenarios.
idx_before_event() {
  local order
  order="$(HOG_STARTUP_LIST_ONLY=1 "$SCRIPT" indexer-before-event)"
  local i e
  i=$(grep -n '^indexer$' <<<"$order" | cut -d: -f1)
  e=$(grep -n '^event$' <<<"$order" | cut -d: -f1)
  [[ "$i" -lt "$e" ]] || {
    echo "FAIL indexer-before-event: indexer($i) not before event($e)"
    fail=1
  }
}
event_before_idx() {
  local order
  order="$(HOG_STARTUP_LIST_ONLY=1 "$SCRIPT" event-before-indexer)"
  local i e
  i=$(grep -n '^indexer$' <<<"$order" | cut -d: -f1)
  e=$(grep -n '^event$' <<<"$order" | cut -d: -f1)
  [[ "$e" -lt "$i" ]] || {
    echo "FAIL event-before-indexer: event($e) not before indexer($i)"
    fail=1
  }
}
idx_before_event
event_before_idx

for s in mempool-first mempool-last mempool-middle indexer-before-event event-before-indexer; do
  check_complete "$s"
done

# Unknown scenario exits 2.
if HOG_STARTUP_LIST_ONLY=1 "$SCRIPT" bogus 2>/dev/null; then
  echo "FAIL bogus scenario should exit nonzero"
  fail=1
else
  echo "ok   bogus scenario rejected"
fi

exit "$fail"
