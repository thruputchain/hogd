#!/usr/bin/env bash
set -euo pipefail

workflow=".github/workflows/x23c_latest_release.yml"

grep -Fq "name: x23c latest release" "$workflow"
grep -Fq "workflow_dispatch:" "$workflow"
grep -Fq "pull_request_target:" "$workflow"
grep -Fq "types: [closed]" "$workflow"
grep -Fq "branches: [main]" "$workflow"
grep -Fq "paths:" "$workflow"
grep -Fq "      - 'x23/x23c/**'" "$workflow"
grep -Fq "if: github.event_name != 'pull_request_target' || github.event.pull_request.merged == true" "$workflow"
grep -Fq 'ref: ${{ github.event.pull_request.merge_commit_sha || github.sha }}' "$workflow"
grep -Fq 'target_commitish: ${{ github.event.pull_request.merge_commit_sha || github.sha }}' "$workflow"
grep -Fq "const sha = process.env.X23C_RELEASE_SHA;" "$workflow"
