#!/usr/bin/env bash
set -euo pipefail

workflow=.github/workflows/x23c_latest_release.yml

test -f "$workflow"

# Require a structural, manual-only trigger block:
# on:
#   workflow_dispatch:
awk '
  BEGIN {
    in_on = 0
    on_found = 0
    trigger_count = 0
    invalid_trigger = 0
  }
  /^on:[[:space:]]*$/ {
    on_found = 1
    in_on = 1
    next
  }
  in_on {
    if ($0 ~ /^[^[:space:]#][^:]*:[[:space:]]*($|#)/) {
      in_on = 0
    } else if ($0 ~ /^[[:space:]]*$/ || $0 ~ /^[[:space:]]*#/) {
      next
    } else if ($0 ~ /^  [A-Za-z_][A-Za-z0-9_-]*:[[:space:]]*($|#)/) {
      key = $0
      sub(/^  /, "", key)
      sub(/:.*/, "", key)
      trigger_count++
      if (key != "workflow_dispatch") {
        invalid_trigger = 1
      }
    }
  }
  END {
    if (!on_found || trigger_count != 1 || invalid_trigger) {
      exit 1
    }
  }
' "$workflow"

grep -qF "concurrency:" "$workflow"
grep -qF "group: x23c-latest-release" "$workflow"
grep -qF "cancel-in-progress: false" "$workflow"
grep -qF "runs-on: namespace-profile-hogd-linux" "$workflow"

# Ensure critical release steps exist and appear in strict order.
awk '
  $0 == "      - name: Build x23c" { if (!build) build = NR }
  $0 == "      - name: Package x23c artifacts" { if (!package) package = NR }
  $0 == "      - name: Publish latest-x23c release" { if (!publish) publish = NR }
  END {
    if (!(build && package && publish &&
          build < package && package < publish)) {
      exit 1
    }
  }
' "$workflow"

if grep -qF "      - name: Delete prior latest-x23c release and tag" "$workflow"; then
  exit 1
fi

extract_step_block() {
  local step_name="$1"
  awk -v step_name="$step_name" '
    BEGIN {
      in_step = 0
      found = 0
    }
    /^      - name: / {
      if (in_step) {
        exit 0
      }
      if ($0 == "      - name: " step_name) {
        in_step = 1
        found = 1
      }
    }
    in_step { print }
    END {
      if (!found) {
        exit 1
      }
    }
  ' "$workflow"
}

build_step_block="$(extract_step_block "Build x23c")"
printf '%s\n' "$build_step_block" | grep -qF "run: ./scripts/build.sh -d build/release -t Release -c gcc x23c"

package_step_block="$(extract_step_block "Package x23c artifacts")"
printf '%s\n' "$package_step_block" | grep -qF "run: ./scripts/package_x23c_latest_release.sh build/release/x23/x23c/x23c artifacts"

publish_step_block="$(extract_step_block "Publish latest-x23c release")"
printf '%s\n' "$publish_step_block" | grep -qF "uses: softprops/action-gh-release@v2"
printf '%s\n' "$publish_step_block" | grep -qF "tag_name: latest-x23c"
printf '%s\n' "$publish_step_block" | grep -qF "name: x23c latest"
printf '%s\n' "$publish_step_block" | grep -qF "body_path: artifacts/release_notes.md"
printf '%s\n' "$publish_step_block" | grep -qF "prerelease: true"
printf '%s\n' "$publish_step_block" | grep -qF "make_latest: false"
