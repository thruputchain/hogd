#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <x23c-binary> <output-dir>" >&2
  exit 1
fi

binary_path=$1
output_dir=$2

if [[ ! -x "$binary_path" ]]; then
  echo "error: missing executable: $binary_path" >&2
  exit 1
fi

mkdir -p "$output_dir"
staging_dir=$(mktemp -d)
trap 'rm -rf "$staging_dir"' EXIT

cp "$binary_path" "$staging_dir/x23c"
chmod +x "$staging_dir/x23c"

archive_path="$output_dir/x23c-linux-x86_64.tar.gz"
tar -C "$staging_dir" -czf "$archive_path" x23c

(
  cd "$output_dir"
  sha256sum x23c-linux-x86_64.tar.gz > SHA256SUMS
)

commit_sha="${GITHUB_SHA:-$(git rev-parse HEAD)}"
build_date="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
run_url=""

if [[ -n "${GITHUB_SERVER_URL:-}" && -n "${GITHUB_REPOSITORY:-}" && -n "${GITHUB_RUN_ID:-}" ]]; then
  run_url="${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}/actions/runs/${GITHUB_RUN_ID}"
fi

{
  echo "# x23c latest"
  echo
  echo "- Source commit: \`${commit_sha}\`"
  echo "- Build date: \`${build_date}\`"
  if [[ -n "$run_url" ]]; then
    echo "- Workflow run: ${run_url}"
  fi
} > "$output_dir/release_notes.md"
