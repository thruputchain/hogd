#!/usr/bin/env bash
set -euo pipefail

workflow=.github/workflows/x23c_latest_release.yml

test -f "$workflow"
grep -qF "workflow_dispatch:" "$workflow"
grep -qF "runs-on: namespace-profile-hogd-linux" "$workflow"
grep -qF "./scripts/build.sh -d build/release -t Release -c gcc x23c" "$workflow"
grep -qF "./scripts/package_x23c_latest_release.sh build/release/x23/x23c/x23c artifacts" "$workflow"
grep -qF "tag_name: latest-x23c" "$workflow"
grep -qF "name: x23c latest" "$workflow"
grep -qF "prerelease: true" "$workflow"
grep -qF "make_latest: false" "$workflow"
grep -qF "actions/github-script@v7" "$workflow"
grep -qF "deleteRelease" "$workflow"
grep -qF 'ref: `tags/${tag}`' "$workflow"
grep -qF "softprops/action-gh-release@v2" "$workflow"
