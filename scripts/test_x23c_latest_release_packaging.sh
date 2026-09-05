#!/usr/bin/env bash
set -euo pipefail

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

binary="$tmpdir/x23c"
cat >"$binary" <<'EOF'
#!/usr/bin/env bash
echo fake-x23c
EOF
chmod +x "$binary"

script="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/package_x23c_latest_release.sh"
artifacts_dir="$tmpdir/artifacts"

if (cd /tmp && "$script" "$binary" "$artifacts_dir") 2>/dev/null; then
  echo "error: expected missing platform argument to fail" >&2
  exit 1
fi

for platform in linux-x86_64 linux-arm64; do
  (cd /tmp && "$script" "$binary" "$artifacts_dir" "$platform")
  test -f "$artifacts_dir/x23c-$platform.tar.gz"
  tar tzf "$artifacts_dir/x23c-$platform.tar.gz" | grep -Fxq "x23c"
done
