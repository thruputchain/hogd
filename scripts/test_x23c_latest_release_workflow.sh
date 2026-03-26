#!/usr/bin/env bash
set -euo pipefail

workflow=.github/workflows/x23c_latest_release.yml

test -f "$workflow"
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
grep -qF "./scripts/build.sh -d build/release -t Release -c gcc x23c" "$workflow"
grep -qF "./scripts/package_x23c_latest_release.sh build/release/x23/x23c/x23c artifacts" "$workflow"
grep -qF "tag_name: latest-x23c" "$workflow"
grep -qF "name: x23c latest" "$workflow"
grep -qF "prerelease: true" "$workflow"
grep -qF "make_latest: false" "$workflow"
grep -qF "actions/github-script@v7" "$workflow"
grep -qF 'const tag = "latest-x23c";' "$workflow"
awk '
  BEGIN {
    in_get_release_call = 0
    get_release_targets_latest = 0
  }
  /getReleaseByTag\(\{/ {
    in_get_release_call = 1
    call_has_target = 0
    next
  }
  in_get_release_call {
    if ($0 ~ /tag[[:space:]]*,/ || $0 ~ /tag:[[:space:]]*"latest-x23c"/) {
      call_has_target = 1
    }
    if ($0 ~ /\}\);/) {
      if (call_has_target) {
        get_release_targets_latest = 1
      }
      in_get_release_call = 0
    }
  }
  END {
    if (!get_release_targets_latest) {
      exit 1
    }
  }
' "$workflow"
grep -qF "deleteRelease" "$workflow"
grep -qF 'ref: `tags/${tag}`' "$workflow"
grep -qF "softprops/action-gh-release@v2" "$workflow"
