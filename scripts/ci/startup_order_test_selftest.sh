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
  local want="auth chuckd event hog-db hog-exec indexer loadgen mempool"
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $scenario component set: got [$got] want [$want]"
    fail=1
  fi
}

check mempool-first "mempool loadgen hog-exec event indexer hog-db auth chuckd"
check mempool-last "loadgen hog-exec event indexer hog-db auth mempool chuckd"
check mempool-middle "loadgen hog-exec event mempool indexer hog-db auth chuckd"
check coordinator-first "mempool loadgen chuckd event indexer hog-exec hog-db auth"
check starve-auth "mempool loadgen hog-exec hog-db event indexer chuckd"
check starve-exec "mempool loadgen hog-db event indexer auth chuckd"
check starve-db "mempool loadgen hog-exec event indexer auth chuckd"
check starve-event "mempool loadgen hog-exec hog-db indexer auth chuckd"

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

# coordinator-first must launch chuckd before each mempool-downstream service
# (auth, hog-exec, hog-db) so the coordinator can drive traffic before they
# register with the mempool.
coordinator_before_downstreams() {
  local order
  order="$(HOG_STARTUP_LIST_ONLY=1 "$SCRIPT" coordinator-first)"
  local c
  c=$(grep -n '^chuckd$' <<<"$order" | cut -d: -f1)
  local dep p
  for dep in auth hog-exec hog-db; do
    p=$(grep -n "^${dep}\$" <<<"$order" | cut -d: -f1)
    [[ "$c" -lt "$p" ]] || {
      echo "FAIL coordinator-first: chuckd($c) not before $dep($p)"
      fail=1
    }
  done
}
coordinator_before_downstreams

# starve-auth intentionally omits auth (and only auth) to starve the mempool's
# verify path: it must contain chuckd and every component except auth.
starve_omits() {
  local scenario="$1" omitted="$2" want="$3"
  local order
  order="$(HOG_STARTUP_LIST_ONLY=1 "$SCRIPT" "$scenario" | sort | tr '\n' ' ' | sed 's/ $//')"
  if [[ "$order" != "$want" ]]; then
    echo "FAIL $scenario set: got [$order] want [$want] ($omitted omitted, all others present)"
    fail=1
  fi
}
starve_omits starve-auth auth "chuckd event hog-db hog-exec indexer loadgen mempool"
starve_omits starve-exec hog-exec "auth chuckd event hog-db indexer loadgen mempool"
starve_omits starve-db hog-db "auth chuckd event hog-exec indexer loadgen mempool"
starve_omits starve-event event "auth chuckd hog-db hog-exec indexer loadgen mempool"

for s in mempool-first mempool-last mempool-middle indexer-before-event event-before-indexer coordinator-first; do
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
