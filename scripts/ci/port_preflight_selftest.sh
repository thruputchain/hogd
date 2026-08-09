#!/usr/bin/env bash
# Self-test for the port preflight in bench_harness.sh: the check that refuses
# to launch a benchmark suite onto ports the previous suite is still holding.
#
# The real check probes a remote benchmark network and asks the host what is
# listening, so the test gives it a stub [nc] on PATH backed by a file of "ports
# in use", and stubs [ci_remote_run] rather than reaching a host.
set -euo pipefail

. "$(dirname "$(readlink -f "$0")")/bench_harness.sh"

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT
fail=0

check_eq() {
  local what=$1 want=$2 got=$3
  if [[ $want == "$got" ]]; then
    echo "ok   $what"
  else
    echo "FAIL $what"
    echo "  want: $want"
    echo "  got:  $got"
    fail=1
  fi
}

check_contains() {
  local what=$1 needle=$2 haystack=$3
  if [[ $haystack == *"$needle"* ]]; then
    echo "ok   $what"
  else
    echo "FAIL $what: missing '$needle' in:"
    echo "$haystack"
    fail=1
  fi
}

check_missing() {
  local what=$1 needle=$2 haystack=$3
  if [[ $haystack != *"$needle"* ]]; then
    echo "ok   $what"
  else
    echo "FAIL $what: unexpected '$needle' in:"
    echo "$haystack"
    fail=1
  fi
}

## Stubs
#
# [nc -z <ip> <port>] succeeds for a port listed as in use, as it does against a
# host where something is listening. A port can be marked as freeing itself
# after a moment, which is the process that was on its way out when the kill
# returned; the file is rewritten by the stub on its second look at that port.

stub_dir="$work_dir/stubs"
mkdir -p "$stub_dir"

cat >"$stub_dir/nc" <<'STUB'
#!/usr/bin/env bash
port=${*: -1}
grep -qxF "$port" "$STUB_STATE/in_use" 2>/dev/null || exit 1
if grep -qxF "$port" "$STUB_STATE/going_away" 2>/dev/null; then
  # Second probe of a port whose process is on its way out: it is gone now.
  grep -vxF "$port" "$STUB_STATE/in_use" >"$STUB_STATE/in_use.tmp" || true
  mv "$STUB_STATE/in_use.tmp" "$STUB_STATE/in_use"
fi
exit 0
STUB
chmod +x "$stub_dir/nc"

# What the host says is listening. The real command is [ss]; only its output
# reaches the report.
ci_remote_run() {
  local host=$1
  shift
  if [[ -e "$STUB_STATE/unreachable" ]]; then
    echo "ssh: connect to host $host: Connection refused" >&2
    return 255
  fi
  echo "LISTEN 0 4096 10.100.0.105:8040 0.0.0.0:* users:((\"hog-exec\",pid=3857552))"
}

# Seeds the stub host and runs the preflight against it. $1 is the list of ports
# in use, $2 the list of those whose process is on its way out, and $3 marks the
# host as one ssh cannot reach.
run_preflight() {
  local in_use=$1
  local going_away=${2:-}
  local unreachable=${3:-}
  local port

  STUB_STATE="$work_dir/stub_state"
  export STUB_STATE
  rm -rf "$STUB_STATE"
  mkdir -p "$STUB_STATE"
  : >"$STUB_STATE/in_use"
  : >"$STUB_STATE/going_away"
  for port in $in_use; do echo "$port" >>"$STUB_STATE/in_use"; done
  for port in $going_away; do echo "$port" >>"$STUB_STATE/going_away"; done
  [[ -z $unreachable ]] || : >"$STUB_STATE/unreachable"

  PATH="$stub_dir:$PATH" CI_PORT_FREE_WAIT_SEC=2 \
    ci_require_ports_free hog5 10.100.0.105 8040 8041 8042 8043 2>&1
}

## A host the kill left clear: nothing answers, so the suite launches.

out=$(run_preflight "") || {
  echo "FAIL preflight should pass on a host with nothing listening"
  fail=1
}
check_eq "clear host passes silently" "" "$out"

## The failure this check exists for. In the run that prompted it, the previous
## suite's hog-exec still held 8040 and 8041 on hog5; this suite's two shards
## died on "bind(10.100.0.105:8040): Address already in use", the stale
## processes answered the readiness waits for both ports in their place, and the
## run failed 600s later waiting on 8042 -- the one port nothing was holding.

out=$(run_preflight "8040 8041") && {
  echo "FAIL preflight should fail while the previous suite holds a port"
  fail=1
}
check_contains "the busy port is named" \
  "preflight: 10.100.0.105:8040 is in use before this run launched anything" \
  "$out"
check_contains "every busy port is named" \
  "preflight: 10.100.0.105:8041 is in use before this run launched anything" \
  "$out"
check_missing "a free port is not named" "10.100.0.105:8042" "$out"
check_contains "the holding process is named" "hog-exec" "$out"

## A process still on its way out when the kill returned is not a failure: it
## goes, and the suite launches.

out=$(run_preflight "8040" "8040") || {
  echo "FAIL preflight should pass once a departing process releases the port"
  fail=1
}
check_missing "a departing process is not reported" "8040 is in use" "$out"

## A host that cannot be asked what is listening must still report the ports:
## failing to explain the failure must not swallow it.

out=$(run_preflight "8040" "" unreachable) && {
  echo "FAIL preflight should fail even when the host cannot be asked"
  fail=1
}
check_contains "unreachable host still names the port" \
  "preflight: 10.100.0.105:8040 is in use" "$out"
check_contains "unreachable host is reported as such" \
  "could not ask hog5 what is listening" "$out"

if ((fail)); then
  echo "port preflight selftest FAILED"
  exit 1
fi

echo "port preflight selftest passed"
