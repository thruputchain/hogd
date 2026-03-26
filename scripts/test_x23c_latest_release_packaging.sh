#!/usr/bin/env bash
set -euo pipefail

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

binary="$tmpdir/x23c"
cat > "$binary" <<'EOF'
#!/usr/bin/env bash
echo fake-x23c
EOF
chmod +x "$binary"

artifacts_dir="$tmpdir/artifacts"
./scripts/package_x23c_latest_release.sh "$binary" "$artifacts_dir"

test -f "$artifacts_dir/x23c-linux-x86_64.tar.gz"
test -f "$artifacts_dir/SHA256SUMS"
test -f "$artifacts_dir/release_notes.md"

tar tzf "$artifacts_dir/x23c-linux-x86_64.tar.gz" | grep -Fxq "x23c"
grep -Fq "x23c-linux-x86_64.tar.gz" "$artifacts_dir/SHA256SUMS"
grep -Fq "Source commit:" "$artifacts_dir/release_notes.md"
grep -Fq "Build date:" "$artifacts_dir/release_notes.md"
