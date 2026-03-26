#!/usr/bin/env bash
set -euo pipefail

workflow=.github/workflows/x23c_latest_release.yml

test -f "$workflow"
grep -F "workflow_dispatch:" "$workflow"
grep -F "runs-on: namespace-profile-hogd-linux" "$workflow"
grep -F "./scripts/build.sh -d build/release -t Release -c gcc x23c" "$workflow"
grep -F "./scripts/package_x23c_latest_release.sh build/release/x23/x23c/x23c artifacts" "$workflow"
grep -F "tag_name: latest-x23c" "$workflow"
grep -F "name: x23c latest" "$workflow"
grep -F "prerelease: true" "$workflow"
grep -F "make_latest: false" "$workflow"
grep -F "actions/github-script@v7" "$workflow"
grep -F "deleteRelease" "$workflow"
grep -F 'ref: `tags/${tag}`' "$workflow"
grep -F "softprops/action-gh-release@v2" "$workflow"
