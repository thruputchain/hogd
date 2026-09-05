#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <x23c-binary> <output-dir> <platform>" >&2
  echo "  platform: linux-x86_64, linux-arm64, macos-arm64, ..." >&2
  exit 1
fi

binary_path=$1
output_dir=$2
platform=$3

if [[ ! -x "$binary_path" ]]; then
  echo "error: missing executable: $binary_path" >&2
  exit 1
fi

mkdir -p "$output_dir"
staging_dir=$(mktemp -d)
trap 'rm -rf "$staging_dir"' EXIT

cp "$binary_path" "$staging_dir/x23c"
chmod +x "$staging_dir/x23c"

archive_members=(x23c)

if [[ "$platform" == macos-* ]]; then
  # Homebrew clang links x23c against @rpath dylibs (libc++, libunwind) that
  # do not exist on end-user machines. Bundle them next to the binary and
  # rewrite the load commands, mirroring the hog-devnode macOS packaging.
  mkdir -p "$staging_dir/lib"

  resolve_dep() {
    local dep="$1"
    local base
    base="$(basename "$dep")"

    if [[ "$dep" == /opt/homebrew/* ]]; then
      printf '%s\n' "$dep"
      return 0
    fi

    if [[ "$dep" == @rpath/* ]]; then
      for search_dir in \
        /opt/homebrew/opt/llvm/lib/c++ \
        /opt/homebrew/opt/llvm/lib/unwind \
        /opt/homebrew/opt/llvm/lib \
        /opt/homebrew/lib; do
        if [ -f "$search_dir/$base" ]; then
          printf '%s\n' "$search_dir/$base"
          return 0
        fi
      done
    fi

    return 1
  }

  list_bundle_deps() {
    otool -L "$1" | awk 'NR > 1 { print $1 }' | while IFS= read -r dep; do
      resolve_dep "$dep" || true
    done
  }

  (
    cd "$staging_dir"

    while true; do
      added=0
      for binary in x23c lib/*.dylib; do
        [ -e "$binary" ] || continue
        while IFS= read -r dep; do
          base="$(basename "$dep")"
          if [ ! -f "lib/$base" ]; then
            cp "$dep" "lib/$base"
            added=1
          fi
        done < <(list_bundle_deps "$binary")
      done
      [ "$added" -eq 0 ] && break
    done

    for dylib in lib/*.dylib; do
      [ -e "$dylib" ] || continue
      install_name_tool -id "@executable_path/lib/$(basename "$dylib")" "$dylib"
    done

    for binary in x23c lib/*.dylib; do
      [ -e "$binary" ] || continue
      while IFS= read -r dep; do
        base="$(basename "$dep")"
        if [ -f "lib/$base" ]; then
          install_name_tool -change "$dep" "@executable_path/lib/$base" "$binary"
        fi
      done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')
    done

    # install_name_tool invalidates the Homebrew dylibs' existing code
    # signatures. Re-sign every rewritten dependency before signing the
    # executable so macOS does not kill the packaged process on launch.
    for dylib in lib/*.dylib; do
      [ -e "$dylib" ] || continue
      codesign --force --sign - "$dylib"
      codesign --verify --verbose=4 "$dylib"
    done
    codesign --force --sign - x23c
    codesign --verify --verbose=4 x23c
  )

  archive_members+=(lib)
fi

"$staging_dir/x23c" --version

archive_path="$output_dir/x23c-${platform}.tar.gz"
tar -C "$staging_dir" -czf "$archive_path" "${archive_members[@]}"
