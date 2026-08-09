#!/usr/bin/env bash
# Self-test for the status build.sh reports to its caller. A configure that
# fails must fail the script, including under [-n], where the caller has no
# other way to tell: it sees a zero status, runs [cmake --build] against a build
# directory with no build system in it, and gets
#
#     ninja: error: loading 'build.ninja': No such file or directory
#
# while cmake's own diagnostic sits unread in a configure log the caller only
# prints when the configure reported failure.
set -euo pipefail

script_dir=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
build_sh="$script_dir/build.sh"

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

fail=0

# Stands in for cmake. Configure invocations (the ones given [-B]) exit with
# [$work_dir/configure_status]; build invocations record that they ran, so a
# configure failure that is wrongly reported as success is visible as a build
# that should never have been attempted.
mkdir -p "$work_dir/bin"
cat >"$work_dir/bin/cmake" <<'STUB'
#!/usr/bin/env bash
for arg in "$@"; do
  if [[ $arg == --build ]]; then
    echo "build" >>"$STUB_DIR/invocations"
    exit 0
  fi
done
echo "configure" >>"$STUB_DIR/invocations"
[[ $(cat "$STUB_DIR/configure_status") -eq 0 ]] || echo 'cmake: needle' >&2
exit "$(cat "$STUB_DIR/configure_status")"
STUB
chmod +x "$work_dir/bin/cmake"

# Runs build.sh against the stub. [-h] keeps ccache out of it, and [-d] keeps
# the build directory in the temporary work dir.
run_build_sh() {
  local configure_status=$1
  shift

  echo "$configure_status" >"$work_dir/configure_status"
  : >"$work_dir/invocations"
  STUB_DIR="$work_dir" PATH="$work_dir/bin:$PATH" \
    "$build_sh" -h -d "$work_dir/build" "$@" 2>&1
}

check_eq() {
  local label="$1" want="$2" got="$3"
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: got [$got] want [$want]"
    fail=1
  else
    echo "ok   $label"
  fi
}

check_contains() {
  local label="$1" needle="$2" haystack="$3"
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "FAIL $label: [$needle] not found in:"
    echo "$haystack"
    fail=1
  else
    echo "ok   $label"
  fi
}

# The failure this exists for: [-n] must not report a failed configure as a
# successful one.
out=$(run_build_sh 3 -n) && status=0 || status=$?
check_eq "-n propagates the configure status" "3" "$status"
check_contains "-n names the configure failure" "Configure failed with exit code 3" \
  "$out"
check_contains "-n leaves cmake's own diagnostic in the output" "cmake: needle" \
  "$out"

# Without [-n] the build must not be attempted against a build directory that
# was never generated.
out=$(run_build_sh 3) && status=0 || status=$?
check_eq "a failed configure propagates without -n" "3" "$status"
check_eq "a failed configure is not followed by a build" "configure" \
  "$(cat "$work_dir/invocations")"

# The paths that were already correct have to stay that way.
out=$(run_build_sh 0 -n) && status=0 || status=$?
check_eq "-n succeeds when the configure succeeds" "0" "$status"
check_eq "-n configures without building" "configure" \
  "$(cat "$work_dir/invocations")"

out=$(run_build_sh 0) && status=0 || status=$?
check_eq "a clean configure builds" "0" "$status"
check_eq "a clean configure builds after configuring" "configure
build" "$(cat "$work_dir/invocations")"
check_contains "a clean build says so" "Build completed successfully" "$out"

if ((fail)); then
  echo "build selftest FAILED"
  exit 1
fi
echo "build selftest passed"
